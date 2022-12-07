/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Corigine, Inc.
 * All rights reserved.
 */

#include <rte_mtr_driver.h>
#include <bus_pci_driver.h>
#include <rte_malloc.h>

#include "nfp_common.h"
#include "nfp_mtr.h"
#include "nfp_logs.h"
#include "flower/nfp_flower.h"
#include "flower/nfp_flower_cmsg.h"
#include "flower/nfp_flower_representor.h"

#define NFP_FL_QOS_PPS          RTE_BIT32(15)
#define NFP_FL_QOS_METER        RTE_BIT32(10)
#define NFP_FL_QOS_RFC2697      RTE_BIT32(0)

/**
 * Callback to get MTR capabilities.
 *
 * @param[in] dev
 *   Pointer to the device (unused).
 * @param[out] cap
 *   Pointer to the meter object capabilities.
 * @param[out] error
 *   Pointer to the error (unused).
 *
 * @returns
 *   0 on success, a negative value otherwise and rte_errno is set.
 */
static int
nfp_mtr_cap_get(struct rte_eth_dev *dev __rte_unused,
		struct rte_mtr_capabilities *cap,
		struct rte_mtr_error *error)
{
	if (cap == NULL) {
		return -rte_mtr_error_set(error, EINVAL,
				RTE_MTR_ERROR_TYPE_UNSPECIFIED,
				NULL, "NULL pointer for capabilitie argument");
	}

	memset(cap, 0, sizeof(struct rte_mtr_capabilities));

	cap->n_max                               = NFP_MAX_MTR_CNT;
	cap->n_shared_max                        = NFP_MAX_MTR_CNT;
	cap->identical                           = 1;
	cap->shared_identical                    = 1;
	cap->chaining_n_mtrs_per_flow_max        = 1;
	cap->meter_srtcm_rfc2697_n_max           = NFP_MAX_MTR_CNT;
	cap->meter_trtcm_rfc2698_n_max           = NFP_MAX_MTR_CNT;
	cap->meter_rate_max                      = UINT64_MAX;
	cap->meter_policy_n_max                  = NFP_MAX_POLICY_CNT;
	cap->srtcm_rfc2697_byte_mode_supported   = 1;
	cap->srtcm_rfc2697_packet_mode_supported = 1;
	cap->trtcm_rfc2698_byte_mode_supported   = 1;
	cap->trtcm_rfc2698_packet_mode_supported = 1;
	cap->stats_mask = RTE_MTR_STATS_N_PKTS_GREEN |
			RTE_MTR_STATS_N_PKTS_DROPPED |
			RTE_MTR_STATS_N_BYTES_GREEN |
			RTE_MTR_STATS_N_BYTES_DROPPED;

	return 0;
}

static int
nfp_mtr_profile_validate(uint32_t mtr_profile_id,
		struct rte_mtr_meter_profile *profile,
		struct rte_mtr_error *error)
{
	/* Profile must not be NULL. */
	if (profile == NULL) {
		return -rte_mtr_error_set(error, EINVAL,
				RTE_MTR_ERROR_TYPE_METER_PROFILE,
				NULL, "Meter profile is null");
	}

	/* Meter profile ID must be valid. */
	if (mtr_profile_id >= NFP_MAX_PROFILE_CNT) {
		return -rte_mtr_error_set(error, EINVAL,
				RTE_MTR_ERROR_TYPE_METER_PROFILE_ID,
				NULL, "Meter profile id not valid");
	}

	switch (profile->alg) {
	case RTE_MTR_SRTCM_RFC2697:
	case RTE_MTR_TRTCM_RFC2698:
		return 0;
	case RTE_MTR_TRTCM_RFC4115:
		return -rte_mtr_error_set(error, ENOTSUP,
				RTE_MTR_ERROR_TYPE_METER_PROFILE,
				NULL, "Unsupported metering algorithm");
	default:
		return -rte_mtr_error_set(error, ENOTSUP,
				RTE_MTR_ERROR_TYPE_METER_PROFILE,
				NULL, "Unknown metering algorithm");
	}
}

static void
nfp_mtr_profile_config_2698(uint32_t mtr_profile_id,
		struct rte_mtr_meter_profile *profile,
		struct nfp_profile_conf *conf)
{
	if (profile->packet_mode != 0)
		conf->head.flags_opts |= rte_cpu_to_be_32(NFP_FL_QOS_PPS);

	conf->head.flags_opts |= rte_cpu_to_be_32(NFP_FL_QOS_METER);
	conf->head.profile_id = rte_cpu_to_be_32(mtr_profile_id);

	conf->bkt_tkn_c = rte_cpu_to_be_32(profile->trtcm_rfc2698.cbs);
	conf->bkt_tkn_p = rte_cpu_to_be_32(profile->trtcm_rfc2698.pbs);
	conf->cbs = rte_cpu_to_be_32(profile->trtcm_rfc2698.cbs);
	conf->pbs = rte_cpu_to_be_32(profile->trtcm_rfc2698.pbs);
	conf->cir = rte_cpu_to_be_32(profile->trtcm_rfc2698.cir);
	conf->pir = rte_cpu_to_be_32(profile->trtcm_rfc2698.pir);
}

static void
nfp_mtr_profile_config_2697(uint32_t mtr_profile_id,
		struct rte_mtr_meter_profile *profile,
		struct nfp_profile_conf *conf)
{
	if (profile->packet_mode != 0)
		conf->head.flags_opts |= rte_cpu_to_be_32(NFP_FL_QOS_PPS);

	conf->head.flags_opts |= rte_cpu_to_be_32(NFP_FL_QOS_RFC2697);
	conf->head.flags_opts |= rte_cpu_to_be_32(NFP_FL_QOS_METER);
	conf->head.profile_id = rte_cpu_to_be_32(mtr_profile_id);

	conf->bkt_tkn_c = rte_cpu_to_be_32(profile->srtcm_rfc2697.cbs);
	conf->bkt_tkn_p = rte_cpu_to_be_32(profile->srtcm_rfc2697.ebs);
	conf->cbs = rte_cpu_to_be_32(profile->srtcm_rfc2697.cbs);
	conf->pbs = rte_cpu_to_be_32(profile->srtcm_rfc2697.ebs);
	conf->cir = rte_cpu_to_be_32(profile->srtcm_rfc2697.cir);
	conf->pir = rte_cpu_to_be_32(profile->srtcm_rfc2697.cir);
}

static int
nfp_mtr_profile_conf_mod(uint32_t mtr_profile_id,
		struct rte_mtr_meter_profile *profile,
		struct nfp_profile_conf *conf)
{
	switch (profile->alg) {
	case RTE_MTR_SRTCM_RFC2697:
		nfp_mtr_profile_config_2697(mtr_profile_id, profile, conf);
		return 0;
	case RTE_MTR_TRTCM_RFC2698:
		nfp_mtr_profile_config_2698(mtr_profile_id, profile, conf);
		return 0;
	case RTE_MTR_TRTCM_RFC4115:
		return -ENOTSUP;
	default:
		return -EINVAL;
	}
}

static int
nfp_mtr_profile_conf_insert(uint32_t mtr_profile_id,
		struct rte_mtr_meter_profile *profile,
		struct nfp_mtr_profile *mtr_profile)
{
	mtr_profile->profile_id = mtr_profile_id;
	mtr_profile->in_use = false;

	return nfp_mtr_profile_conf_mod(mtr_profile_id, profile,
			&mtr_profile->conf);
}

static struct nfp_mtr_profile *
nfp_mtr_profile_search(struct nfp_mtr_priv *priv, uint32_t mtr_profile_id)
{
	struct nfp_mtr_profile *mtr_profile;

	LIST_FOREACH(mtr_profile, &priv->profiles, next)
		if (mtr_profile->profile_id == mtr_profile_id)
			break;

	return mtr_profile;
}

static int
nfp_mtr_profile_insert(struct nfp_app_fw_flower *app_fw_flower,
		struct rte_mtr_meter_profile *profile,
		uint32_t mtr_profile_id,
		struct rte_mtr_error *error)
{
	int ret;
	struct nfp_mtr_priv *priv;
	struct nfp_mtr_profile *mtr_profile;

	priv = app_fw_flower->mtr_priv;

	/* Meter profile memory allocation. */
	mtr_profile = rte_zmalloc(NULL, sizeof(struct nfp_mtr_profile), 0);
	if (mtr_profile == NULL) {
		return -rte_mtr_error_set(error, ENOMEM,
				RTE_MTR_ERROR_TYPE_UNSPECIFIED,
				NULL, "Meter profile alloc failed");
	}

	ret = nfp_mtr_profile_conf_insert(mtr_profile_id,
			profile, mtr_profile);
	if (ret != 0) {
		rte_mtr_error_set(error, EINVAL,
				RTE_MTR_ERROR_TYPE_UNSPECIFIED,
				NULL, "Insert profile config failed");
		goto free_profile;
	}

	ret = nfp_flower_cmsg_qos_add(app_fw_flower, &mtr_profile->conf);
	if (ret != 0) {
		rte_mtr_error_set(error, EINVAL,
				RTE_MTR_ERROR_TYPE_UNSPECIFIED,
				NULL, "Add meter to firmware failed");
		goto free_profile;
	}

	/* Insert profile into profile list */
	LIST_INSERT_HEAD(&priv->profiles, mtr_profile, next);

	return 0;

free_profile:
	rte_free(mtr_profile);

	return ret;
}

static int
nfp_mtr_profile_mod(struct nfp_app_fw_flower *app_fw_flower,
		struct rte_mtr_meter_profile *profile,
		struct nfp_mtr_profile *mtr_profile,
		struct rte_mtr_error *error)
{
	int ret;
	struct nfp_profile_conf old_conf;

	/* Get the old profile config */
	rte_memcpy(&old_conf, &mtr_profile->conf, sizeof(old_conf));

	ret = nfp_mtr_profile_conf_mod(mtr_profile->profile_id,
			profile, &mtr_profile->conf);
	if (ret != 0) {
		rte_mtr_error_set(error, EINVAL,
				RTE_MTR_ERROR_TYPE_UNSPECIFIED,
				NULL, "Mod profile config failed");
		goto rollback;
	}

	ret = nfp_flower_cmsg_qos_add(app_fw_flower, &mtr_profile->conf);
	if (ret != 0) {
		rte_mtr_error_set(error, EINVAL,
				RTE_MTR_ERROR_TYPE_UNSPECIFIED,
				NULL, "Mod meter to firmware failed");
		goto rollback;
	}

	return 0;

rollback:
	rte_memcpy(&mtr_profile->conf, &old_conf, sizeof(old_conf));

	return ret;
}

/**
 * Callback to add MTR profile.
 *
 * @param[in] dev
 *   Pointer to Ethernet device.
 * @param[in] mtr_profile_id
 *   Meter profile id.
 * @param[in] profile
 *   Pointer to meter profile detail.
 * @param[out] error
 *   Pointer to the error structure.
 *
 * @return
 *   0 on success, a negative value otherwise and rte_errno is set.
 */
static int
nfp_mtr_profile_add(struct rte_eth_dev *dev,
		uint32_t mtr_profile_id,
		struct rte_mtr_meter_profile *profile,
		struct rte_mtr_error *error)
{
	int ret;
	struct nfp_mtr_priv *priv;
	struct nfp_mtr_profile *mtr_profile;
	struct nfp_app_fw_flower *app_fw_flower;
	struct nfp_flower_representor *representor;

	representor = dev->data->dev_private;
	app_fw_flower = representor->app_fw_flower;
	priv = app_fw_flower->mtr_priv;

	/* Check input params */
	ret = nfp_mtr_profile_validate(mtr_profile_id, profile, error);
	if (ret != 0)
		return ret;

	/* Check if mtr profile id exist */
	mtr_profile = nfp_mtr_profile_search(priv, mtr_profile_id);
	if (mtr_profile == NULL) {
		ret = nfp_mtr_profile_insert(app_fw_flower,
				profile, mtr_profile_id, error);
	} else {
		ret = nfp_mtr_profile_mod(app_fw_flower,
				profile, mtr_profile, error);
	}

	return ret;
}

/**
 * Callback to delete MTR profile.
 *
 * @param[in] dev
 *   Pointer to Ethernet device.
 * @param[in] mtr_profile_id
 *   Meter profile id.
 * @param[out] error
 *   Pointer to the error structure.
 *
 * @return
 *   0 on success, a negative value otherwise and rte_errno is set.
 */
static int
nfp_mtr_profile_delete(struct rte_eth_dev *dev,
		uint32_t mtr_profile_id,
		struct rte_mtr_error *error)
{
	int ret;
	struct nfp_mtr_priv *priv;
	struct nfp_mtr_profile *mtr_profile;
	struct nfp_app_fw_flower *app_fw_flower;
	struct nfp_flower_representor *representor;

	representor = dev->data->dev_private;
	app_fw_flower = representor->app_fw_flower;
	priv = app_fw_flower->mtr_priv;

	/* Check if mtr profile id exist */
	mtr_profile = nfp_mtr_profile_search(priv, mtr_profile_id);
	if (mtr_profile == NULL) {
		return -rte_mtr_error_set(error, EINVAL,
				RTE_MTR_ERROR_TYPE_METER_PROFILE_ID,
				NULL, "Request meter profile not exist");
	}

	if (mtr_profile->in_use) {
		return -rte_mtr_error_set(error, EINVAL,
				RTE_MTR_ERROR_TYPE_METER_PROFILE,
				NULL, "Request meter profile is been used");
	}

	ret = nfp_flower_cmsg_qos_delete(app_fw_flower, &mtr_profile->conf);
	if (ret != 0) {
		return -rte_mtr_error_set(error, EINVAL,
				RTE_MTR_ERROR_TYPE_UNSPECIFIED,
				NULL, "Delete meter from firmware failed");
	}

	/* Remove profile from profile list */
	LIST_REMOVE(mtr_profile, next);
	rte_free(mtr_profile);

	return 0;
}

static const struct rte_mtr_ops nfp_mtr_ops = {
	.capabilities_get      = nfp_mtr_cap_get,
	.meter_profile_add     = nfp_mtr_profile_add,
	.meter_profile_delete  = nfp_mtr_profile_delete,
};

int
nfp_net_mtr_ops_get(struct rte_eth_dev *dev, void *arg)
{
	if ((dev->data->dev_flags & RTE_ETH_DEV_REPRESENTOR) == 0) {
		PMD_DRV_LOG(ERR, "Port is not a representor");
		return -EINVAL;
	}

	*(const struct rte_mtr_ops **)arg = &nfp_mtr_ops;

	return 0;
}

int
nfp_mtr_priv_init(struct nfp_pf_dev *pf_dev)
{
	struct nfp_mtr_priv *priv;
	struct nfp_app_fw_flower *app_fw_flower;

	priv = rte_zmalloc("nfp_app_mtr_priv", sizeof(struct nfp_mtr_priv), 0);
	if (priv == NULL) {
		PMD_INIT_LOG(ERR, "nfp app mtr priv creation failed");
		return -ENOMEM;
	}

	app_fw_flower = NFP_PRIV_TO_APP_FW_FLOWER(pf_dev->app_fw_priv);
	app_fw_flower->mtr_priv = priv;

	LIST_INIT(&priv->profiles);

	return 0;
}

void
nfp_mtr_priv_uninit(struct nfp_pf_dev *pf_dev)
{
	struct nfp_mtr_priv *priv;
	struct nfp_mtr_profile *mtr_profile;
	struct nfp_app_fw_flower *app_fw_flower;

	app_fw_flower = NFP_PRIV_TO_APP_FW_FLOWER(pf_dev->app_fw_priv);
	priv = app_fw_flower->mtr_priv;

	LIST_FOREACH(mtr_profile, &priv->profiles, next) {
		LIST_REMOVE(mtr_profile, next);
		rte_free(mtr_profile);
	}

	rte_free(priv);
}
