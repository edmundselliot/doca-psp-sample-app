/*
 * Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES, ALL RIGHTS RESERVED.
 *
 * This software product is a proprietary product of NVIDIA CORPORATION &
 * AFFILIATES (the "Company") and all right, title, and interest in and to the
 * software product, including all associated intellectual property rights, are
 * and shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 *
 */

#include <string>
#include <vector>

#include <doca_flow.h>
#include <doca_flow_crypto.h>
#include <doca_log.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ip.h>

#include "psp_gw_config.h"
#include "psp_gw_flows.h"
#include "psp_gw_utils.h"

#define IF_SUCCESS(result, expr) \
	if (result == DOCA_SUCCESS) { \
		result = expr; \
		if (likely(result == DOCA_SUCCESS)) { \
			DOCA_LOG_DBG("Success: %s", #expr); \
		} else { \
			DOCA_LOG_ERR("Error: %s: %s", #expr, doca_error_get_descr(result)); \
		} \
	} else { /* skip this expr */ \
	}

DOCA_LOG_REGISTER(PSP_GATEWAY);

static const uint32_t DEFAULT_TIMEOUT_US = 10000; /* default timeout for processing entries */
static const uint32_t PSP_ICV_SIZE = 16;

/**
 * @brief user context struct that will be used in entries process callback
 */
struct entries_status {
	bool failure;	      /* will be set to true if some entry status will not be success */
	int nb_processed;     /* number of entries that was already processed */
	int entries_in_queue; /* number of entries in queue that is waiting to process */
};

/**
 * @brief packet header structure to simplify populating the encap_data array
 */
struct eth_ipv6_psp_tunnel_hdr {
	// encapped Ethernet header contents.
	doca_flow_header_eth eth;

	// encapped IP header contents (extension header not supported)
	rte_ipv6_hdr ip;

	rte_udp_hdr udp;

	// encapped PSP header contents.
	rte_psp_base_hdr psp;
	rte_be64_t psp_virt_cookie;

} __rte_packed __rte_aligned(2);

const uint8_t PSP_SAMPLE_ENABLE = 1 << 7;

PSP_GatewayFlows::PSP_GatewayFlows(psp_pf_dev *pf, uint16_t vf_port_id, psp_gw_app_config *app_config)
	: app_config(app_config),
	  pf_dev(pf),
	  vf_port_id(vf_port_id),
	  sampling_enabled(app_config->log2_sample_rate > 0)
{
	monitor_count.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
}

PSP_GatewayFlows::~PSP_GatewayFlows()
{
	if (vf_port)
		doca_flow_port_stop(vf_port);

	if (pf_dev->port_obj)
		doca_flow_port_stop(pf_dev->port_obj);

	doca_flow_destroy();
}

doca_error_t PSP_GatewayFlows::init(void)
{
	doca_error_t result = DOCA_SUCCESS;

	IF_SUCCESS(result, init_doca_flow(app_config));
	IF_SUCCESS(result, start_port(pf_dev->port_id, pf_dev->dev, &pf_dev->port_obj));
	IF_SUCCESS(result, start_port(vf_port_id, nullptr, &vf_port));
	IF_SUCCESS(result, bind_shared_resources());
	IF_SUCCESS(result, rss_pipe_create());
	IF_SUCCESS(result, configure_mirrors());
	IF_SUCCESS(result, create_pipes());

	return result;
}

doca_error_t PSP_GatewayFlows::configure_mirrors(void)
{
	assert(rss_pipe);
	doca_error_t result = DOCA_SUCCESS;

	doca_flow_mirror_target mirr_tgt = {};
	mirr_tgt.fwd.type = DOCA_FLOW_FWD_PIPE;
	mirr_tgt.fwd.next_pipe = rss_pipe;

	struct doca_flow_shared_resource_cfg res_cfg = {};
	res_cfg.domain = DOCA_FLOW_PIPE_DOMAIN_EGRESS;
	res_cfg.mirror_cfg.nr_targets = 1;
	res_cfg.mirror_cfg.target = &mirr_tgt;

	IF_SUCCESS(result, doca_flow_shared_resource_cfg(DOCA_FLOW_SHARED_RESOURCE_MIRROR, mirror_res_id, &res_cfg));

	IF_SUCCESS(
		result,
		doca_flow_shared_resources_bind(DOCA_FLOW_SHARED_RESOURCE_MIRROR, &mirror_res_id, 1, pf_dev->port_obj));

	doca_flow_mirror_target mirr_tgt_port = {};
	mirr_tgt_port.fwd.type = DOCA_FLOW_FWD_PORT;
	mirr_tgt_port.fwd.port_id = pf_dev->port_id;

	res_cfg.mirror_cfg.target = &mirr_tgt_port;

	IF_SUCCESS(result,
		   doca_flow_shared_resource_cfg(DOCA_FLOW_SHARED_RESOURCE_MIRROR, mirror_res_id_port, &res_cfg));
	IF_SUCCESS(result,
		   doca_flow_shared_resources_bind(DOCA_FLOW_SHARED_RESOURCE_MIRROR,
						   &mirror_res_id_port,
						   1,
						   pf_dev->port_obj));

	return result;
}

doca_error_t PSP_GatewayFlows::start_port(uint16_t port_id, doca_dev *port_dev, doca_flow_port **port)
{
	doca_flow_port_cfg *port_cfg;
	doca_error_t result = DOCA_SUCCESS;

	IF_SUCCESS(result, doca_flow_port_cfg_create(&port_cfg));

	std::string port_id_str = std::to_string(port_id); // note that set_devargs() clones the string contents
	IF_SUCCESS(result, doca_flow_port_cfg_set_devargs(port_cfg, port_id_str.c_str()));
	IF_SUCCESS(result, doca_flow_port_cfg_set_dev(port_cfg, port_dev));
	IF_SUCCESS(result, doca_flow_port_start(port_cfg, port));

	if (result == DOCA_SUCCESS) {
		rte_ether_addr port_mac_addr;
		rte_eth_macaddr_get(port_id, &port_mac_addr);
		DOCA_LOG_INFO("Started port_id %d, mac-addr: %s", port_id, mac_to_string(port_mac_addr).c_str());
	}

	if (port_cfg) {
		doca_flow_port_cfg_destroy(port_cfg);
	}
	return result;
}

doca_error_t PSP_GatewayFlows::init_doca_flow(const psp_gw_app_config *app_cfg)
{
	doca_error_t result = DOCA_SUCCESS;

	uint16_t nb_queues = app_cfg->dpdk_config.port_config.nb_queues;

	uint16_t rss_queues[nb_queues];
	for (int i = 0; i < nb_queues; i++)
		rss_queues[i] = i;
	doca_flow_resource_rss_cfg rss_config = {};
	rss_config.nr_queues = nb_queues;
	rss_config.queues_array = rss_queues;

	/* init doca flow with crypto shared resources */
	doca_flow_cfg *flow_cfg;
	IF_SUCCESS(result, doca_flow_cfg_create(&flow_cfg));
	IF_SUCCESS(result, doca_flow_cfg_set_pipe_queues(flow_cfg, nb_queues));
	IF_SUCCESS(result, doca_flow_cfg_set_nr_counters(flow_cfg, app_cfg->max_tunnels * NUM_OF_PSP_SYNDROMES + 10));
	IF_SUCCESS(result, doca_flow_cfg_set_mode_args(flow_cfg, "switch,hws,isolated,expert"));
	IF_SUCCESS(result, doca_flow_cfg_set_cb_entry_process(flow_cfg, PSP_GatewayFlows::check_for_valid_entry));
	IF_SUCCESS(result, doca_flow_cfg_set_default_rss(flow_cfg, &rss_config));
	IF_SUCCESS(result,
		   doca_flow_cfg_set_nr_shared_resource(flow_cfg,
							app_cfg->max_tunnels + 1,
							DOCA_FLOW_SHARED_RESOURCE_PSP));
	IF_SUCCESS(result, doca_flow_cfg_set_nr_shared_resource(flow_cfg, 3, DOCA_FLOW_SHARED_RESOURCE_MIRROR));
	IF_SUCCESS(result, doca_flow_init(flow_cfg));
	if (result == DOCA_SUCCESS) {
		DOCA_LOG_INFO("Initialized DOCA Flow for a max of %d tunnels", app_cfg->max_tunnels);
	}
	if (flow_cfg) {
		doca_flow_cfg_destroy(flow_cfg);
	}
	return result;
}

doca_error_t PSP_GatewayFlows::bind_shared_resources(void)
{
	doca_error_t result = DOCA_SUCCESS;

	std::vector<uint32_t> psp_ids(app_config->max_tunnels);
	for (uint32_t i = 0; i < app_config->max_tunnels; i++) {
		psp_ids[i] = i + 1;
	}

	IF_SUCCESS(result,
		   doca_flow_shared_resources_bind(DOCA_FLOW_SHARED_RESOURCE_PSP,
						   psp_ids.data(),
						   app_config->max_tunnels,
						   pf_dev->port_obj));

	return result;
}

doca_error_t PSP_GatewayFlows::create_pipes(void)
{
	doca_error_t result = DOCA_SUCCESS;

	IF_SUCCESS(result, syndrome_stats_pipe_create());

	if (app_config->enable_packet_spray) {
		IF_SUCCESS(result, ingress_packet_spray_pipe_create());
	}

	IF_SUCCESS(result, ingress_acl_pipe_create());

	if (sampling_enabled) {
		IF_SUCCESS(result, ingress_sampling_pipe_create());
	}

	IF_SUCCESS(result, ingress_decrypt_pipe_create());

	if (sampling_enabled) {
		IF_SUCCESS(result, empty_pipe_create_not_sampled());
		IF_SUCCESS(result, egress_sampling_pipe_create());
	}
	IF_SUCCESS(result, egress_acl_pipe_create());

	if (app_config->enable_packet_spray) {
		IF_SUCCESS(result, egress_packet_spray_pipe_create());
	} else {
		IF_SUCCESS(result, empty_pipe_create(egress_acl_pipe));
	}

	IF_SUCCESS(result, ingress_root_pipe_create());

	return result;
}

doca_error_t PSP_GatewayFlows::rss_pipe_create(void)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	doca_error_t result = DOCA_SUCCESS;

	doca_flow_match empty_match = {};

	// Note packets sent to RSS will be processed by lcore_pkt_proc_func().
	uint16_t rss_queues[1] = {0};
	doca_flow_fwd fwd = {};
	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_IPV6;
	fwd.num_of_queues = 1;
	fwd.rss_queues = rss_queues;

	doca_flow_pipe_cfg *pipe_cfg;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "RSS_PIPE"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &empty_match, nullptr));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd, nullptr, &rss_pipe));
	IF_SUCCESS(
		result,
		add_single_entry(0, rss_pipe, pf_dev->port_obj, nullptr, nullptr, nullptr, nullptr, &default_rss_entry));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	return result;
}

doca_error_t PSP_GatewayFlows::ingress_decrypt_pipe_create(void)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	assert(sampling_enabled ? ingress_sampling_pipe : ingress_acl_pipe);
	assert(rss_pipe);
	doca_error_t result = DOCA_SUCCESS;

	doca_flow_match match = {};
	match.parser_meta.port_meta = UINT32_MAX;
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV6;
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	match.outer.udp.l4_port.dst_port = RTE_BE16(DOCA_FLOW_PSP_DEFAULT_PORT);

	doca_flow_actions actions = {};
	actions.crypto.action_type = DOCA_FLOW_CRYPTO_ACTION_DECRYPT;
	actions.crypto.resource_type = DOCA_FLOW_CRYPTO_RESOURCE_PSP;
	actions.crypto.crypto_id = DOCA_FLOW_PSP_DECRYPTION_ID;

	doca_flow_actions *actions_arr[] = {&actions};

	doca_flow_fwd fwd = {};
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = sampling_enabled ? ingress_sampling_pipe : ingress_acl_pipe;

	doca_flow_fwd fwd_miss = {};
	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	doca_flow_pipe_cfg *pipe_cfg;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "PSP_DECRYPT"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_INGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_dir_info(pipe_cfg, DOCA_FLOW_DIRECTION_NETWORK_TO_HOST));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, nullptr));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, nullptr, nullptr, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, &ingress_decrypt_pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	doca_flow_match match_uplink = {};
	match_uplink.parser_meta.port_meta = 0;

	IF_SUCCESS(result,
		   add_single_entry(0,
				    ingress_decrypt_pipe,
				    pf_dev->port_obj,
				    &match_uplink,
				    &actions,
				    nullptr,
				    nullptr,
				    &default_decrypt_entry));

	return result;
}

doca_error_t PSP_GatewayFlows::ingress_sampling_pipe_create(void)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	assert(mirror_res_id);
	assert(rss_pipe);
	assert(sampling_enabled);
	doca_error_t result = DOCA_SUCCESS;

	doca_flow_match match_psp_sampling_bit = {};
	match_psp_sampling_bit.tun.type = DOCA_FLOW_TUN_PSP;
	match_psp_sampling_bit.tun.psp.s_d_ver_v = PSP_SAMPLE_ENABLE;

	doca_flow_monitor mirror_action = {};
	mirror_action.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	mirror_action.shared_mirror_id = mirror_res_id;

	doca_flow_actions set_meta = {};
	set_meta.meta.pkt_meta = app_config->ingress_sample_meta_indicator;

	doca_flow_actions *actions_arr[] = {&set_meta};

	doca_flow_actions set_meta_mask = {};
	set_meta_mask.meta.pkt_meta = UINT32_MAX;

	doca_flow_actions *actions_masks_arr[] = {&set_meta_mask};

	doca_flow_fwd fwd_and_miss = {};
	fwd_and_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_and_miss.next_pipe = ingress_acl_pipe;

	doca_flow_pipe_cfg *pipe_cfg;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "INGR_SAMPL"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, actions_masks_arr, nullptr, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match_psp_sampling_bit, &match_psp_sampling_bit));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, actions_masks_arr, nullptr, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &mirror_action));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd_and_miss, &fwd_and_miss, &ingress_sampling_pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	IF_SUCCESS(result,
		   add_single_entry(0,
				    ingress_sampling_pipe,
				    pf_dev->port_obj,
				    nullptr,
				    nullptr,
				    nullptr,
				    nullptr,
				    &default_ingr_sampling_entry));

	return result;
}

doca_error_t PSP_GatewayFlows::ingress_packet_spray_pipe_create(void)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	assert(app_config->enable_packet_spray);
	assert(app_config->local_vf_addr.length());
	doca_error_t result = DOCA_SUCCESS;

	rte_be32_t local_vf_addr;
	if (inet_pton(AF_INET, app_config->local_vf_addr.c_str(), &local_vf_addr) != 1) {
		DOCA_LOG_ERR("Invalid local_vf_addr: %s", app_config->local_vf_addr.c_str());
		return DOCA_ERROR_INVALID_VALUE;
	}

	doca_flow_match match = {};

	doca_flow_actions actions = {};
	actions.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	actions.outer.ip4.dst_ip = UINT32_MAX;

	doca_flow_actions *actions_arr[] = {&actions};

	doca_flow_fwd fwd = {};
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = vf_port_id;

	doca_flow_pipe_cfg *pipe_cfg;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "INGR_SPRAY"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_dir_info(pipe_cfg, DOCA_FLOW_DIRECTION_NETWORK_TO_HOST));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, nullptr));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, nullptr, nullptr, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd, nullptr, &ingress_packet_spray_pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	actions.outer.ip4.dst_ip = local_vf_addr;
	IF_SUCCESS(result,
		   add_single_entry(0,
				    ingress_packet_spray_pipe,
				    pf_dev->port_obj,
				    nullptr,
				    &actions,
				    nullptr,
				    nullptr,
				    &default_ingr_packet_spray_entry));

	return result;
}

doca_error_t PSP_GatewayFlows::ingress_acl_pipe_create(void)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	doca_error_t result = DOCA_SUCCESS;
	doca_flow_match match = {};
	match.parser_meta.psp_syndrome = UINT8_MAX;
	if (!app_config->disable_ingress_acl) {
		match.tun.type = DOCA_FLOW_TUN_PSP;
		match.tun.psp.spi = UINT32_MAX;
		match.inner.l3_type = DOCA_FLOW_L3_TYPE_IP4;
		match.inner.ip4.src_ip = UINT32_MAX;
	}

	doca_flow_actions actions = {};
	actions.has_crypto_encap = true;
	actions.crypto_encap.action_type = DOCA_FLOW_CRYPTO_REFORMAT_DECAP;
	actions.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_PSP_TUNNEL;
	actions.crypto_encap.icv_size = PSP_ICV_SIZE;
	actions.crypto_encap.data_size = sizeof(rte_ether_hdr);

	rte_ether_hdr *eth_hdr = (rte_ether_hdr *)actions.crypto_encap.encap_data;
	eth_hdr->ether_type = RTE_BE16(RTE_ETHER_TYPE_IPV4);
	eth_hdr->src_addr = pf_dev->src_mac;
	eth_hdr->dst_addr = app_config->dcap_dmac;

	doca_flow_actions *actions_arr[] = {&actions};

	doca_flow_fwd fwd = {};
	if (app_config->enable_packet_spray) {
		fwd.type = DOCA_FLOW_FWD_PIPE;
		fwd.next_pipe = ingress_packet_spray_pipe;
	} else {
		fwd.type = DOCA_FLOW_FWD_PORT;
		fwd.port_id = vf_port_id;
	}

	doca_flow_fwd fwd_miss = {};
	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = syndrome_stats_pipe;

	int nr_entries = app_config->disable_ingress_acl ? 1 : app_config->max_tunnels;

	doca_flow_pipe_cfg *pipe_cfg;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "INGR_ACL"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, nr_entries));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_dir_info(pipe_cfg, DOCA_FLOW_DIRECTION_NETWORK_TO_HOST));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, nullptr, nullptr, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, nullptr));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, &ingress_acl_pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	if (app_config->disable_ingress_acl) {
		doca_flow_match match_no_syndrome = {};
		IF_SUCCESS(result,
			   add_single_entry(0,
					    ingress_acl_pipe,
					    pf_dev->port_obj,
					    &match_no_syndrome,
					    &actions,
					    nullptr,
					    nullptr,
					    &default_ingr_acl_entry));
	}

	return result;
}

doca_error_t PSP_GatewayFlows::add_ingress_acl_entry(psp_session_t *session)
{
	if (app_config->disable_ingress_acl) {
		DOCA_LOG_ERR("Cannot insert ingress ACL flow; disabled");
		return DOCA_ERROR_BAD_STATE;
	}

	doca_flow_match match = {};
	match.parser_meta.psp_syndrome = 0;
	match.tun.type = DOCA_FLOW_TUN_PSP;
	match.tun.psp.spi = RTE_BE32(session->spi_ingress);
	match.inner.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.inner.ip4.src_ip = session->src_vip;

	doca_error_t result = DOCA_SUCCESS;
	IF_SUCCESS(result,
		   add_single_entry(0,
				    ingress_acl_pipe,
				    pf_dev->port_obj,
				    &match,
				    nullptr,
				    nullptr,
				    nullptr,
				    &session->acl_entry));

	return result;
}

doca_error_t PSP_GatewayFlows::syndrome_stats_pipe_create(void)
{
	doca_error_t result = DOCA_SUCCESS;

	doca_flow_match syndrome_match = {};
	syndrome_match.parser_meta.psp_syndrome = 0xff;

	// If we got here, the packet failed either the PSP decryption syndrome check
	// or the ACL check. Whether the syndrome bits match here or not, the
	// fate of the packet is to drop.
	doca_flow_fwd fwd_drop = {};
	fwd_drop.type = DOCA_FLOW_FWD_DROP;

	doca_flow_pipe_cfg *pipe_cfg;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "SYNDROME_STATS"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_INGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, NUM_OF_PSP_SYNDROMES));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_dir_info(pipe_cfg, DOCA_FLOW_DIRECTION_NETWORK_TO_HOST));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &syndrome_match, nullptr));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd_drop, &fwd_drop, &syndrome_stats_pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	for (int i = 0; i < NUM_OF_PSP_SYNDROMES; i++) {
		syndrome_match.parser_meta.psp_syndrome = 1 << i;
		IF_SUCCESS(result,
			   add_single_entry(0,
					    syndrome_stats_pipe,
					    pf_dev->port_obj,
					    &syndrome_match,
					    nullptr,
					    &monitor_count,
					    nullptr,
					    &syndrome_stats_entries[i]));
	}

	return result;
}

doca_error_t PSP_GatewayFlows::egress_packet_spray_pipe_create(void)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	assert(app_config->enable_packet_spray);
	assert(egress_acl_pipe);
	assert(!app_config->net_config.hosts.empty());
	doca_error_t result = DOCA_SUCCESS;

	doca_flow_match match = {};
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	match.outer.udp.l4_port.src_port = UINT16_MAX;

	doca_flow_actions actions = {};
	actions.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	actions.outer.ip4.dst_ip = UINT32_MAX;

	doca_flow_actions *actions_arr[] = {&actions};

	doca_flow_fwd fwd = {};
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = egress_acl_pipe;

	doca_flow_pipe_cfg *pipe_cfg;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "EGR_SPRAY"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_HASH));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, app_config->max_tunnels));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_dir_info(pipe_cfg, DOCA_FLOW_DIRECTION_HOST_TO_NETWORK));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, &match));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, nullptr, nullptr, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd, nullptr, &egress_packet_spray_pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	for (size_t i = 0; i < app_config->max_tunnels && result == DOCA_SUCCESS; i++) {
		size_t host_idx = i % app_config->net_config.hosts.size();
		const auto &host = app_config->net_config.hosts[host_idx];
		actions.outer.ip4.dst_ip = host.vip;

		doca_flow_pipe_entry *entry;

		int num_of_entries = 1;
		doca_flow_flags_type flags = DOCA_FLOW_NO_WAIT;

		entries_status status = {};
		status.entries_in_queue = num_of_entries;

		std::string dst_ip = ipv4_to_string(actions.outer.ip4.dst_ip);
		DOCA_LOG_DBG("Egress hash 0x%x -> %s", (uint32_t)i, dst_ip.c_str());
		IF_SUCCESS(result,
			   doca_flow_pipe_hash_add_entry(0,
							 egress_packet_spray_pipe,
							 i,
							 &actions,
							 &monitor_count,
							 nullptr,
							 flags,
							 &status,
							 &entry));

		IF_SUCCESS(result, doca_flow_entries_process(pf_dev->port_obj, 0, DEFAULT_TIMEOUT_US, num_of_entries));

		egr_packet_spray_entries.push_back(entry);

		if (status.nb_processed != num_of_entries || status.failure) {
			DOCA_LOG_ERR("Failed to process entry; nb_processed = %d, failure = %d",
				     status.nb_processed,
				     status.failure);
			result = DOCA_ERROR_BAD_STATE;
		}
	}

	return result;
}

doca_error_t PSP_GatewayFlows::egress_acl_pipe_create(void)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	assert(rss_pipe);
	assert(!sampling_enabled || egress_sampling_pipe);
	doca_error_t result = DOCA_SUCCESS;

	doca_flow_match match = {};
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.dst_ip = UINT32_MAX;

	doca_flow_actions actions = {};
	actions.has_crypto_encap = true;
	actions.crypto_encap.action_type = DOCA_FLOW_CRYPTO_REFORMAT_ENCAP;
	actions.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_PSP_TUNNEL;
	actions.crypto_encap.icv_size = PSP_ICV_SIZE;
	actions.crypto_encap.data_size = sizeof(eth_ipv6_psp_tunnel_hdr);

	if (!app_config->net_config.vc_enabled) {
		actions.crypto_encap.data_size -= sizeof(uint64_t);
	}
	memset(actions.crypto_encap.encap_data, 0xff, actions.crypto_encap.data_size);

	actions.crypto.action_type = DOCA_FLOW_CRYPTO_ACTION_ENCRYPT;
	actions.crypto.resource_type = DOCA_FLOW_CRYPTO_RESOURCE_PSP;
	actions.crypto.crypto_id = UINT32_MAX; // per entry

	doca_flow_actions *actions_arr[] = {&actions};

	doca_flow_fwd fwd_to_sampling = {};
	fwd_to_sampling.type = DOCA_FLOW_FWD_PIPE;
	fwd_to_sampling.next_pipe = egress_sampling_pipe;

	doca_flow_fwd fwd_to_wire = {};
	fwd_to_wire.type = DOCA_FLOW_FWD_PORT;
	fwd_to_wire.port_id = pf_dev->port_id;

	auto p_fwd = sampling_enabled ? &fwd_to_sampling : &fwd_to_wire;

	doca_flow_fwd fwd_miss = {};
	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = rss_pipe;

	doca_flow_pipe_cfg *pipe_cfg;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "EGR_ACL"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_EGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, app_config->max_tunnels));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_dir_info(pipe_cfg, DOCA_FLOW_DIRECTION_HOST_TO_NETWORK));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, nullptr));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, nullptr, nullptr, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, p_fwd, &fwd_miss, &egress_acl_pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	return result;
}

doca_error_t PSP_GatewayFlows::add_encrypt_entry(psp_session_t *session, const void *encrypt_key)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	doca_error_t result = DOCA_SUCCESS;
	std::string dst_pip = ipv6_to_string(session->dst_pip);
	std::string dst_vip = ipv4_to_string(session->dst_vip);

	DOCA_LOG_INFO("Creating encrypt flow entry: dst_pip %s, dst_vip %s, SPI %d, crypto_id %d",
		      dst_pip.c_str(),
		      dst_vip.c_str(),
		      session->spi_egress,
		      session->crypto_id);

	struct doca_flow_shared_resource_cfg res_cfg = {};
	res_cfg.domain = DOCA_FLOW_PIPE_DOMAIN_SECURE_EGRESS;
	res_cfg.psp_cfg.key_cfg.key_type = session->psp_proto_ver == 0 ? DOCA_FLOW_CRYPTO_KEY_128 :
									 DOCA_FLOW_CRYPTO_KEY_256;
	res_cfg.psp_cfg.key_cfg.key = (uint32_t *)encrypt_key;

	result = doca_flow_shared_resource_cfg(DOCA_FLOW_SHARED_RESOURCE_PSP, session->crypto_id, &res_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure crypto_id %d: %s", session->crypto_id, doca_error_get_descr(result));
		return result;
	}

	doca_flow_match encap_encrypt_match = {};
	encap_encrypt_match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	encap_encrypt_match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	encap_encrypt_match.outer.ip4.dst_ip = session->dst_vip;

	doca_flow_actions encap_actions = {};
	encap_actions.has_crypto_encap = true;
	encap_actions.crypto_encap.action_type = DOCA_FLOW_CRYPTO_REFORMAT_ENCAP;
	encap_actions.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_PSP_TUNNEL;
	encap_actions.crypto_encap.icv_size = PSP_ICV_SIZE;
	encap_actions.crypto_encap.data_size = sizeof(eth_ipv6_psp_tunnel_hdr);

	if (!app_config->net_config.vc_enabled) {
		encap_actions.crypto_encap.data_size -= sizeof(uint64_t);
	}
	format_encap_data(session, encap_actions.crypto_encap.encap_data);

	encap_actions.crypto.action_type = DOCA_FLOW_CRYPTO_ACTION_ENCRYPT;
	encap_actions.crypto.resource_type = DOCA_FLOW_CRYPTO_RESOURCE_PSP;
	encap_actions.crypto.crypto_id = session->crypto_id;

	result = add_single_entry(0,
				  egress_acl_pipe,
				  pf_dev->port_obj,
				  &encap_encrypt_match,
				  &encap_actions,
				  nullptr,
				  nullptr,
				  &session->encap_encrypt_entry);

	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add encrypt_encap pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_DBG("Created session entry: %p", session->encap_encrypt_entry);

	session->pkt_count_egress = UINT64_MAX; // force next query to detect a change

	return result;
}

void PSP_GatewayFlows::format_encap_data(const psp_session_t *session, uint8_t *encap_data)
{
	static const doca_be32_t DEFAULT_VTC_FLOW = 0x6 << 28;

	auto *encap_hdr = (eth_ipv6_psp_tunnel_hdr *)encap_data;
	encap_hdr->eth.type = RTE_BE16(DOCA_FLOW_ETHER_TYPE_IPV6);
	encap_hdr->ip.vtc_flow = RTE_BE32(DEFAULT_VTC_FLOW);
	encap_hdr->ip.proto = IPPROTO_UDP;
	encap_hdr->ip.hop_limits = 50;
	encap_hdr->udp.src_port = 0x0; // computed
	encap_hdr->udp.dst_port = RTE_BE16(DOCA_FLOW_PSP_DEFAULT_PORT);
	encap_hdr->psp.nexthdr = 4;
	encap_hdr->psp.hdrextlen = (uint8_t)(app_config->net_config.vc_enabled ? 2 : 1);
	encap_hdr->psp.res_cryptofst = (uint8_t)app_config->net_config.crypt_offset;
	encap_hdr->psp.spi = RTE_BE32(session->spi_egress);
	encap_hdr->psp_virt_cookie = RTE_BE64(session->vc);

	const auto &dmac = app_config->nexthop_enable ? app_config->nexthop_dmac : session->dst_mac;
	memcpy(encap_hdr->eth.src_mac, pf_dev->src_mac.addr_bytes, DOCA_FLOW_ETHER_ADDR_LEN);
	memcpy(encap_hdr->eth.dst_mac, dmac.addr_bytes, DOCA_FLOW_ETHER_ADDR_LEN);
	memcpy(encap_hdr->ip.src_addr, pf_dev->src_pip, IPV6_ADDR_LEN);
	memcpy(encap_hdr->ip.dst_addr, session->dst_pip, IPV6_ADDR_LEN);
	encap_hdr->psp.rsrv1 = 1; // always 1
	encap_hdr->psp.ver = session->psp_proto_ver;
	encap_hdr->psp.v = !!app_config->net_config.vc_enabled;
	// encap_hdr->psp.s will be set by the egress_sampling pipe
}

doca_error_t PSP_GatewayFlows::remove_encrypt_entry(psp_session_t *session)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	doca_error_t result = DOCA_SUCCESS;
	uint16_t pipe_queue = 0;
	uint32_t flags = DOCA_FLOW_NO_WAIT;
	uint32_t num_of_entries = 1;

	result = doca_flow_pipe_rm_entry(pipe_queue, flags, session->encap_encrypt_entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_INFO("Error removing PSP encap entry: %s", doca_error_get_descr(result));
	}

	result = doca_flow_entries_process(pf_dev->port_obj, 0, DEFAULT_TIMEOUT_US, num_of_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process entry: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

doca_error_t PSP_GatewayFlows::egress_sampling_pipe_create(void)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	assert(sampling_enabled);

	doca_error_t result = DOCA_SUCCESS;

	uint16_t mask = (uint16_t)((1 << app_config->log2_sample_rate) - 1);
	DOCA_LOG_DBG("Sampling: matching (rand & 0x%x) == 1", mask);

	doca_flow_match match_sampling_match_mask = {};
	match_sampling_match_mask.parser_meta.random = mask;

	doca_flow_match match_sampling_match = {};
	match_sampling_match.parser_meta.random = 0x1;

	doca_flow_actions set_sample_bit = {};
	set_sample_bit.meta.pkt_meta = app_config->egress_sample_meta_indicator;
	set_sample_bit.tun.type = DOCA_FLOW_TUN_PSP;
	set_sample_bit.tun.psp.s_d_ver_v = PSP_SAMPLE_ENABLE;
	doca_flow_actions *actions_arr[] = {&set_sample_bit};

	doca_flow_actions actions_mask = {};
	actions_mask.meta.pkt_meta = UINT32_MAX;
	actions_mask.tun.type = DOCA_FLOW_TUN_PSP;
	actions_mask.tun.psp.s_d_ver_v = PSP_SAMPLE_ENABLE;
	doca_flow_actions *actions_masks_arr[] = {&actions_mask};

	doca_flow_monitor mirror_action = {};
	mirror_action.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	mirror_action.shared_mirror_id = mirror_res_id_port;

	doca_flow_fwd fwd_miss = {}; /* going through an empty pipe that will forward to port */
	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = empty_pipe_not_sampled;

	doca_flow_fwd fwd_rss = {};
	fwd_rss.type = DOCA_FLOW_FWD_PIPE;
	fwd_rss.next_pipe = rss_pipe;

	doca_flow_pipe_cfg *pipe_cfg;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "EGR_SAMPL"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_EGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_miss_counter(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match_sampling_match, &match_sampling_match_mask));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, actions_masks_arr, nullptr, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &mirror_action));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd_rss, &fwd_miss, &egress_sampling_pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	IF_SUCCESS(result,
		   add_single_entry(0,
				    egress_sampling_pipe,
				    pf_dev->port_obj,
				    nullptr,
				    nullptr,
				    nullptr,
				    nullptr,
				    &default_egr_sampling_entry));

	return result;
}

doca_error_t PSP_GatewayFlows::empty_pipe_create(doca_flow_pipe *next_pipe)
{
	doca_error_t result = DOCA_SUCCESS;
	doca_flow_match match = {};

	doca_flow_fwd fwd = {};
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = next_pipe;

	doca_flow_pipe_cfg *pipe_cfg;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "EMPTY"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_is_root(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, nullptr));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd, nullptr, &empty_pipe));
	IF_SUCCESS(
		result,
		add_single_entry(0, empty_pipe, pf_dev->port_obj, nullptr, nullptr, nullptr, nullptr, &empty_pipe_entry));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	return result;
}

doca_error_t PSP_GatewayFlows::empty_pipe_create_not_sampled(void)
{
	doca_error_t result = DOCA_SUCCESS;
	doca_flow_match match = {};

	doca_flow_fwd fwd = {};
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = pf_dev->port_id;

	doca_flow_pipe_cfg *pipe_cfg;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "EMPTY_NOT_SAMPLED"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_is_root(pipe_cfg, false));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 1));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_match(pipe_cfg, &match, nullptr));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor_count));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, &fwd, nullptr, &empty_pipe_not_sampled));
	IF_SUCCESS(result,
		   add_single_entry(0,
				    empty_pipe_not_sampled,
				    pf_dev->port_obj,
				    nullptr,
				    nullptr,
				    nullptr,
				    nullptr,
				    &empty_pipe_entry));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	return result;
}

doca_error_t PSP_GatewayFlows::ingress_root_pipe_create(void)
{
	DOCA_LOG_DBG("\n>> %s", __FUNCTION__);
	assert(ingress_decrypt_pipe);
	assert(egress_acl_pipe);
	doca_error_t result = DOCA_SUCCESS;

	doca_flow_pipe_cfg *pipe_cfg;
	IF_SUCCESS(result, doca_flow_pipe_cfg_create(&pipe_cfg, pf_dev->port_obj));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_name(pipe_cfg, "ROOT"));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_CONTROL));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_is_root(pipe_cfg, true));
	IF_SUCCESS(result, doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, 3));
	IF_SUCCESS(result, doca_flow_pipe_create(pipe_cfg, nullptr, nullptr, &ingress_root_pipe));

	if (pipe_cfg) {
		doca_flow_pipe_cfg_destroy(pipe_cfg);
	}

	// Note outer_l4_ok can be matched with spec=true, mask=UINT8_MAX to
	// restrict traffic to TCP/UDP (ICMP would miss to RSS).
	doca_flow_match mask = {};
	mask.parser_meta.port_meta = UINT32_MAX;
	mask.parser_meta.outer_l3_ok = UINT8_MAX;
	mask.parser_meta.outer_ip4_checksum_ok = UINT8_MAX;
	mask.outer.eth.type = UINT16_MAX;

	doca_flow_match ipv6_from_uplink = {};
	ipv6_from_uplink.parser_meta.port_meta = pf_dev->port_id;
	ipv6_from_uplink.parser_meta.outer_l3_ok = true;
	ipv6_from_uplink.parser_meta.outer_ip4_checksum_ok = false;
	ipv6_from_uplink.outer.eth.type = RTE_BE16(RTE_ETHER_TYPE_IPV6);

	doca_flow_match ipv4_from_vf = {};
	ipv4_from_vf.parser_meta.port_meta = vf_port_id;
	ipv4_from_vf.parser_meta.outer_l3_ok = true;
	ipv4_from_vf.parser_meta.outer_ip4_checksum_ok = true;
	ipv4_from_vf.outer.eth.type = RTE_BE16(RTE_ETHER_TYPE_IPV4);

	doca_flow_match arp_mask = {};
	arp_mask.parser_meta.port_meta = UINT32_MAX;
	arp_mask.outer.eth.type = UINT16_MAX;

	doca_flow_match arp_from_vf = {};
	arp_from_vf.parser_meta.port_meta = vf_port_id;
	arp_from_vf.outer.eth.type = RTE_BE16(RTE_ETHER_TYPE_ARP);

	doca_flow_fwd fwd_ingress = {};
	fwd_ingress.type = DOCA_FLOW_FWD_PIPE;
	fwd_ingress.next_pipe = ingress_decrypt_pipe;

	doca_flow_fwd fwd_egress = {};
	fwd_egress.type = DOCA_FLOW_FWD_PIPE;
	fwd_egress.next_pipe = app_config->enable_packet_spray ? egress_packet_spray_pipe :
								 empty_pipe; // and then to egress_acl_pipe

	doca_flow_fwd fwd_rss = {};
	fwd_rss.type = DOCA_FLOW_FWD_PIPE;
	fwd_rss.next_pipe = rss_pipe;

	uint16_t pipe_queue = 0;

	IF_SUCCESS(result,
		   doca_flow_pipe_control_add_entry(pipe_queue,
						    1,
						    ingress_root_pipe,
						    &ipv6_from_uplink,
						    &mask,
						    nullptr,
						    nullptr,
						    nullptr,
						    nullptr,
						    &monitor_count,
						    &fwd_ingress,
						    nullptr,
						    &root_jump_to_ingress_entry));

	IF_SUCCESS(result,
		   doca_flow_pipe_control_add_entry(pipe_queue,
						    2,
						    ingress_root_pipe,
						    &ipv4_from_vf,
						    &mask,
						    nullptr,
						    nullptr,
						    nullptr,
						    nullptr,
						    &monitor_count,
						    &fwd_egress,
						    nullptr,
						    &root_jump_to_egress_entry));

	IF_SUCCESS(result,
		   doca_flow_pipe_control_add_entry(pipe_queue,
						    3,
						    ingress_root_pipe,
						    &arp_from_vf,
						    &arp_mask,
						    nullptr,
						    nullptr,
						    nullptr,
						    nullptr,
						    &monitor_count,
						    &fwd_rss,
						    nullptr,
						    &vf_arp_to_rss));

	return result;
}

/*
 * Entry processing callback
 *
 * @entry [in]: entry pointer
 * @pipe_queue [in]: queue identifier
 * @status [in]: DOCA Flow entry status
 * @op [in]: DOCA Flow entry operation
 * @user_ctx [out]: user context
 */
void PSP_GatewayFlows::check_for_valid_entry(doca_flow_pipe_entry *entry,
					     uint16_t pipe_queue,
					     enum doca_flow_entry_status status,
					     enum doca_flow_entry_op op,
					     void *user_ctx)
{
	(void)entry;
	(void)op;
	(void)pipe_queue;

	auto *entry_status = (entries_status *)user_ctx;

	if (entry_status == NULL || op != DOCA_FLOW_ENTRY_OP_ADD)
		return;

	if (status != DOCA_FLOW_ENTRY_STATUS_SUCCESS)
		entry_status->failure = true; /* set failure to true if processing failed */

	entry_status->nb_processed++;
	entry_status->entries_in_queue--;
}

doca_error_t PSP_GatewayFlows::add_single_entry(uint16_t pipe_queue,
						doca_flow_pipe *pipe,
						doca_flow_port *port,
						const doca_flow_match *match,
						const doca_flow_actions *actions,
						const doca_flow_monitor *mon,
						const doca_flow_fwd *fwd,
						doca_flow_pipe_entry **entry)
{
	int num_of_entries = 1;
	uint32_t flags = DOCA_FLOW_NO_WAIT;

	entries_status status = {};
	status.entries_in_queue = num_of_entries;

	doca_error_t result =
		doca_flow_pipe_add_entry(pipe_queue, pipe, match, actions, mon, fwd, flags, &status, entry);

	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, num_of_entries);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process entry: %s", doca_error_get_descr(result));
		return result;
	}

	if (status.nb_processed != num_of_entries || status.failure) {
		DOCA_LOG_ERR("Failed to process entry; nb_processed = %d, failure = %d",
			     status.nb_processed,
			     status.failure);
		return DOCA_ERROR_BAD_STATE;
	}

	return result;
}

struct PSP_GatewayFlows::pipe_query {
	doca_flow_pipe *pipe;	     // used to query misses
	doca_flow_pipe_entry *entry; // used to query static entries
	std::string name;	     // displays the pipe name
};

std::pair<uint64_t, uint64_t> PSP_GatewayFlows::perform_pipe_query(pipe_query *query, bool suppress_output)
{
	uint64_t new_hits = 0;
	uint64_t new_misses = 0;

	if (query->entry) {
		doca_flow_query stats = {};
		doca_error_t result = doca_flow_query_entry(query->entry, &stats);
		if (result == DOCA_SUCCESS) {
			new_hits = stats.total_pkts;
		}
	}
	if (query->pipe) {
		doca_flow_query stats = {};
		doca_error_t result = doca_flow_query_pipe_miss(query->pipe, &stats);
		if (result == DOCA_SUCCESS) {
			new_misses = stats.total_pkts;
		}
	}
	if (!suppress_output) {
		if (query->entry && query->pipe) {
			DOCA_LOG_INFO("%s: %ld hits %ld misses", query->name.c_str(), new_hits, new_misses);
		} else if (query->entry) {
			DOCA_LOG_INFO("%s: %ld hits", query->name.c_str(), new_hits);
		} else if (query->pipe) {
			DOCA_LOG_INFO("%s: %ld misses", query->name.c_str(), new_hits);
		}
	}

	return std::make_pair(new_hits, new_misses);
}

void PSP_GatewayFlows::show_static_flow_counts(void)
{
	std::vector<pipe_query> queries;
	queries.emplace_back(pipe_query{nullptr, default_rss_entry, "rss_pipe"});
	queries.emplace_back(pipe_query{nullptr, root_jump_to_ingress_entry, "root_jump_to_ingress_entry"});
	queries.emplace_back(pipe_query{nullptr, root_jump_to_egress_entry, "root_jump_to_egress_entry"});
	queries.emplace_back(pipe_query{ingress_decrypt_pipe, default_decrypt_entry, "ingress_decrypt_pipe"});
	queries.emplace_back(pipe_query{ingress_sampling_pipe, default_ingr_sampling_entry, "ingress_sampling_pipe"});
	queries.emplace_back(pipe_query{ingress_acl_pipe, default_ingr_acl_entry, "ingress_acl_pipe"});
	queries.emplace_back(pipe_query{nullptr, default_ingr_packet_spray_entry, "ingress_pkt_spray"});

	for (int i = 0; i < NUM_OF_PSP_SYNDROMES; i++) {
		queries.emplace_back(
			pipe_query{nullptr, syndrome_stats_entries[i], "syndrome[" + std::to_string(i) + "]"});
	}
	if (false && app_config->enable_packet_spray) {
		for (size_t i = 0; i < egr_packet_spray_entries.size(); i++) {
			queries.emplace_back(pipe_query{nullptr,
							egr_packet_spray_entries[i],
							"egr_spray[" + std::to_string(i) + "]"});
		}
	}
	queries.emplace_back(pipe_query{nullptr, empty_pipe_entry, "egress_root"});
	queries.emplace_back(pipe_query{egress_acl_pipe, nullptr, "egress_acl_pipe"});
	queries.emplace_back(pipe_query{egress_sampling_pipe, default_egr_sampling_entry, "egress_sampling_pipe"});

	uint64_t total_pkts = 0;
	for (auto &query : queries) {
		auto hits_misses = perform_pipe_query(&query, true);
		total_pkts += hits_misses.first + hits_misses.second;
	}

	if (total_pkts != prev_static_flow_count) {
		total_pkts = 0;
		DOCA_LOG_INFO("-------------------------");
		for (auto &query : queries) {
			auto hits_misses = perform_pipe_query(&query, false);
			total_pkts += hits_misses.first + hits_misses.second;
		}
		prev_static_flow_count = total_pkts;
	}
}

void PSP_GatewayFlows::show_session_flow_count(const std::string &dst_vip, psp_session_t &session)
{
	if (session.encap_encrypt_entry) {
		doca_flow_query encap_encrypt_stats = {};
		doca_error_t encap_result = doca_flow_query_entry(session.encap_encrypt_entry, &encap_encrypt_stats);

		if (encap_result == DOCA_SUCCESS) {
			if (session.pkt_count_egress != encap_encrypt_stats.total_pkts) {
				DOCA_LOG_DBG("Session Egress entries: %p", session.encap_encrypt_entry);
				DOCA_LOG_INFO("Session Egress flow %s: %ld hits",
					      dst_vip.c_str(),
					      encap_encrypt_stats.total_pkts);
				session.pkt_count_egress = encap_encrypt_stats.total_pkts;
			}
		} else {
			DOCA_LOG_INFO("Session Egress flow %s: query failed: %s",
				      dst_vip.c_str(),
				      doca_error_get_descr(encap_result));
		}
	}

	if (!app_config->disable_ingress_acl && session.acl_entry) {
		std::string src_vip = ipv4_to_string(session.src_vip);
		doca_flow_query acl_stats = {};
		doca_error_t result = doca_flow_query_entry(session.acl_entry, &acl_stats);

		if (result == DOCA_SUCCESS) {
			if (session.pkt_count_ingress != acl_stats.total_pkts) {
				DOCA_LOG_DBG("Session ACL entry: %p", session.acl_entry);
				DOCA_LOG_INFO("Session Ingress flow %s: %ld hits",
					      src_vip.c_str(),
					      acl_stats.total_pkts);
				session.pkt_count_ingress = acl_stats.total_pkts;
			}
		} else {
			DOCA_LOG_INFO("Session Ingress flow %s: query failed: %s",
				      src_vip.c_str(),
				      doca_error_get_descr(result));
		}
	}
}
