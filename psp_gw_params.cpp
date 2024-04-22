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

#include <ctype.h>
#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_log.h>
#include <fstream>
#include <sstream>

#include <psp_gw_config.h>
#include <psp_gw_params.h>

DOCA_LOG_REGISTER(PSP_Gateway_Params);

/**
 * @brief Configures the dst-mac to apply on decap
 *
 * @param [in]: the dst mac addr
 * @config [in/out]: A void pointer to the application config struct
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static doca_error_t handle_pci_addr_param(void *param, void *config)
{
	auto *app_config = (struct psp_gw_app_config *)config;
	const char *pci_addr = (const char *)param;

	int pci_addr_len = strlen(pci_addr);
	if (pci_addr_len + 1 != DOCA_DEVINFO_PCI_ADDR_SIZE && pci_addr_len + 1 != DOCA_DEVINFO_PCI_BDF_SIZE) {
		DOCA_LOG_ERR("Expected PCI addr in DDDD:BB:DD.F or BB:DD.F format");
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_config->pf_pcie_addr = pci_addr;
	for (char &c : app_config->pf_pcie_addr) {
		c = tolower(c);
	}

	DOCA_LOG_INFO("Using %s for PF PCIe Addr", app_config->pf_pcie_addr.c_str());
	return DOCA_SUCCESS;
}

/**
 * @brief Configures the dst-mac to apply on decap
 *
 * @param [in]: the dst mac addr
 * @config [in/out]: A void pointer to the application config struct
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static doca_error_t handle_repr_param(void *param, void *config)
{
	auto *app_config = (struct psp_gw_app_config *)config;

	app_config->pf_repr_indices = (char *)param;

	DOCA_LOG_INFO("Device representor list: %s", app_config->pf_repr_indices.c_str());
	return DOCA_SUCCESS;
}

/**
 * @brief Configures the DPDK eal_init Core mask parameter
 *
 * @param [in]: the core mask string
 * @config [in/out]: A void pointer to the application config struct
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static doca_error_t handle_core_mask_param(void *param, void *config)
{
	auto *app_config = (struct psp_gw_app_config *)config;

	app_config->core_mask = (char *)param;

	DOCA_LOG_INFO("RTE EAL core-mask: %s", app_config->core_mask.c_str());
	return DOCA_SUCCESS;
}

/**
 * @brief Configures the dst-mac to apply on decap
 *
 * @param [in]: the dst mac addr
 * @config [in/out]: A void pointer to the application config struct
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static doca_error_t handle_decap_dmac_param(void *param, void *config)
{
	auto *app_config = (struct psp_gw_app_config *)config;
	char *mac_addr = (char *)param;

	if (rte_ether_unformat_addr(mac_addr, &app_config->dcap_dmac) != 0) {
		DOCA_LOG_ERR("Malformed mac addr: %s", mac_addr);
		return DOCA_ERROR_INVALID_VALUE;
	}

	DOCA_LOG_INFO("Decap dmac: %s", mac_addr);
	return DOCA_SUCCESS;
}

/**
 * @brief Configures the next-hop dst-mac to apply on encap
 *
 * @param [in]: the dst mac addr
 * @config [in/out]: A void pointer to the application config struct
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static doca_error_t handle_nexthop_dmac_param(void *param, void *config)
{
	auto *app_config = (struct psp_gw_app_config *)config;
	char *mac_addr = (char *)param;

	if (rte_ether_unformat_addr(mac_addr, &app_config->nexthop_dmac) != 0) {
		DOCA_LOG_ERR("Malformed mac addr: %s", mac_addr);
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_config->nexthop_enable = true;

	DOCA_LOG_INFO("Next-Hop dmac: %s", mac_addr);
	return DOCA_SUCCESS;
}

/**
 * @brief Parses a tunnel specifier for a remote host
 *
 * @fields [in]: A comma-separated string containing the following:
 * - rpc_ipv4_addr
 * - virt_ipv4_addr
 * @host [out]: The host data structure to populate
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static bool parse_host_param(char *fields, struct psp_gw_host *host)
{
	char *svcaddr = strtok_r(fields, ",", &fields);
	char *virt_ip = strtok_r(fields, ",", &fields);
	char *extra_field_check = strtok_r(fields, ",", &fields); // expect null

	if (!svcaddr || !virt_ip || extra_field_check) {
		DOCA_LOG_ERR("Tunnel host requires 2 args: svc_ip,vip");
		return false;
	}
	if (inet_pton(AF_INET, svcaddr, &host->svc_ip) != 1) {
		DOCA_LOG_ERR("Invalid svc IPv4 addr: %s", svcaddr);
		return false;
	}
	if (inet_pton(AF_INET, virt_ip, &host->vip) != 1) {
		DOCA_LOG_ERR("Invalid virtual IPv4 addr: %s", virt_ip);
		return false;
	}
	return true;
}

/**
 * @brief Adds a tunnel specifier for a given remote host
 *
 * @param [in]: A string of the form described in parse_host_param()
 * @config [in/out]: A void pointer to the application config struct
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static doca_error_t handle_host_param(void *param, void *config)
{
	auto *app_config = (struct psp_gw_app_config *)config;
	char *host_params = (char *)param;
	struct psp_gw_host host = {};

	// note commas in host_params are replaced by null character
	if (!parse_host_param(host_params, &host)) {
		return DOCA_ERROR_INVALID_VALUE; // details already logged
	}

	app_config->net_config.hosts.push_back(host);

	DOCA_LOG_INFO("Added Host %d: %s",
		      (int)app_config->net_config.hosts.size(),
		      host_params); // just the svc addr, due to strtok
	return DOCA_SUCCESS;
}

/**
 * @brief Parses a single line of the tunnels configuration file.
 *
 * @line [in]: The line of text to parse
 * @app_config [in/out]: The configuration to update
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static doca_error_t handle_tunnels_file_line(const std::string &line, psp_gw_app_config *app_config)
{
	DOCA_LOG_DBG("%s", line.c_str());
	if (line.length() == 0 || line[0] == '#') {
		return DOCA_SUCCESS;
	}

	size_t sep = line.find(':');
	if (sep == 0 || sep == std::string::npos) {
		DOCA_LOG_ERR("Incorrect file format; expected host:virt-addr1,virt-addr2,...");
		return DOCA_ERROR_INVALID_VALUE;
	}

	struct psp_gw_host host = {};

	std::string svcaddr = line.substr(0, sep);
	if (inet_pton(AF_INET, svcaddr.c_str(), &host.svc_ip) != 1) {
		DOCA_LOG_ERR("Invalid svc IPv4 addr: %s", svcaddr.c_str());
		return DOCA_ERROR_INVALID_VALUE;
	}

	std::istringstream hosts;
	hosts.str(line.substr(sep + 1));
	for (std::string virt_ip; std::getline(hosts, virt_ip, ',');) {
		if (inet_pton(AF_INET, virt_ip.c_str(), &host.vip) != 1) {
			DOCA_LOG_ERR("Invalid virtual IPv4 addr: %s", virt_ip.c_str());
			return DOCA_ERROR_INVALID_VALUE;
		}

		app_config->net_config.hosts.push_back(host);

		DOCA_LOG_INFO("Added Host %d: %s at %s",
			      (int)app_config->net_config.hosts.size(),
			      virt_ip.c_str(),
			      svcaddr.c_str());
	}

	return DOCA_SUCCESS;
}

/**
 * @brief Reads a text file to gather the controller service addresses
 * for all peer virtual addresses.
 *
 * @param [in]: Filename containing the hosts
 * @config [in/out]: A void pointer to the application config struct
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static doca_error_t handle_tunnels_file_param(void *param, void *config)
{
	auto *app_config = (struct psp_gw_app_config *)config;
	char *filename = (char *)param;

	std::ifstream in{filename};
	if (!in.good()) {
		DOCA_LOG_ERR("Failed to open tunnels file");
		return DOCA_ERROR_NOT_FOUND;
	}

	for (std::string line; std::getline(in, line);) {
		doca_error_t result = handle_tunnels_file_line(line, app_config);
		if (result != DOCA_SUCCESS) {
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/**
 * @brief Indicates the preferred socket address of the gRPC server
 *
 * @param [in]: A string containing an IPv4 address and optionally
 *              a colon character and port number
 * @config [in/out]: A void pointer to the application config struct
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static doca_error_t handle_svc_addr_param(void *param, void *config)
{
	auto *app_config = (struct psp_gw_app_config *)config;
	app_config->local_svc_addr = (char *)param;

	DOCA_LOG_INFO("Selected local Svc Addr: %s", app_config->local_svc_addr.c_str());
	return DOCA_SUCCESS;
}

/**
 * @brief Indicates the application should include the VC in the PSP tunnel header
 *
 * @param [in]: A pointer to a boolean flag
 * @config [in/out]: A void pointer to the application config struct
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static doca_error_t handle_vc_param(void *param, void *config)
{
	auto *app_config = (struct psp_gw_app_config *)config;
	bool *bool_param = (bool *)param;
	app_config->net_config.vc_enabled = *bool_param;
	DOCA_LOG_INFO("PSP VCs %s", *bool_param ? "Enabled" : "Disabled");
	return DOCA_SUCCESS;
}

/**
 * @brief Indicates the application should execute benchmarks
 *
 * @param [in]: A pointer to a boolean flag
 * @config [in/out]: A void pointer to the application config struct
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static doca_error_t handle_benchmark_param(void *param, void *config)
{
	auto *app_config = (struct psp_gw_app_config *)config;
	bool *bool_param = (bool *)param;
	app_config->run_benchmarks_and_exit = *bool_param;
	DOCA_LOG_INFO("PSP Benchmarking %s", *bool_param ? "Enabled" : "Disabled");
	return DOCA_SUCCESS;
}

/**
 * @brief Indicates the application should enable packet spray across SPIs
 *
 * @param [in]: A pointer to a boolean flag
 * @config [in/out]: A void pointer to the application config struct
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static doca_error_t handle_spray_param(void *param, void *config)
{
	auto *app_config = (struct psp_gw_app_config *)config;
	bool *bool_param = (bool *)param;
	app_config->enable_packet_spray = *bool_param;
	DOCA_LOG_INFO("Enable packet spray %s", *bool_param ? "Enabled" : "Disabled");
	return DOCA_SUCCESS;
}

/**
 * @brief Indicates the application should skip ACL checks on ingress
 *
 * @param [in]: A pointer to a boolean flag
 * @config [in/out]: A void pointer to the application config struct
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static doca_error_t handle_ingress_acl_param(void *param, void *config)
{
	auto *app_config = (struct psp_gw_app_config *)config;
	bool *bool_param = (bool *)param;
	app_config->disable_ingress_acl = *bool_param;
	DOCA_LOG_INFO("Ingress ACLs %s", *bool_param ? "Enabled" : "Disabled");
	return DOCA_SUCCESS;
}

/**
 * @brief Configures the sampling rate of packets
 *
 * @param [in]: The log2 rate; see log2_sample_rate
 * @config [in/out]: A void pointer to the application config struct
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static doca_error_t handle_sample_param(void *param, void *config)
{
	auto *app_config = (struct psp_gw_app_config *)config;
	int32_t *int_param = (int32_t *)param;
	app_config->log2_sample_rate = (uint16_t)*int_param;
	DOCA_LOG_INFO("The log2_sample_rate is set to %d", app_config->log2_sample_rate);
	return DOCA_SUCCESS;
}

/**
 * @brief Indicates the application should create all PSP tunnels at startup
 *
 * @param [in]: A pointer to a boolean flag
 * @config [in/out]: A void pointer to the application config struct
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static doca_error_t handle_static_tunnels_param(void *param, void *config)
{
	auto *app_config = (struct psp_gw_app_config *)config;
	app_config->local_vf_addr = (char *)param;

	rte_be32_t test_local_vf_addr = 0;
	if (inet_pton(AF_INET, app_config->local_vf_addr.c_str(), &test_local_vf_addr) != 1) {
		DOCA_LOG_ERR("Malformed local VF IP addr param: %s", app_config->local_vf_addr.c_str());
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_config->create_tunnels_at_startup = true;
	DOCA_LOG_INFO("Create PSP tunnels at startup: %s",
		      app_config->create_tunnels_at_startup ? "Enabled" : "Disabled");
	DOCA_LOG_INFO("Create PSP tunnels using VF IP addr: %s", app_config->local_vf_addr.c_str());

	return DOCA_SUCCESS;
}

/**
 * @brief Configures the max number of tunnels to be supported.
 *
 * @param [in]: A pointer to the parameter
 * @config [in/out]: A void pointer to the application config struct
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static doca_error_t handle_max_tunnels_param(void *param, void *config)
{
	auto *app_config = (struct psp_gw_app_config *)config;
	int *int_param = (int *)param;
	if (*int_param < 1) {
		DOCA_LOG_ERR("The max-tunnels cannot be less than one");
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_config->max_tunnels = *int_param;
	DOCA_LOG_INFO("Configured max-tunnels = %d", app_config->max_tunnels);

	return DOCA_SUCCESS;
}

/**
 * @brief Configures the PSP crypt-offset, the number of words in
 * the packet header transmitted as cleartext.
 *
 * @param [in]: A pointer to the parameter
 * @config [in/out]: A void pointer to the application config struct
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static doca_error_t handle_psp_crypt_offset_param(void *param, void *config)
{
	auto *app_config = (struct psp_gw_app_config *)config;
	int *int_param = (int *)param;
	if (*int_param < 0 || *int_param >= 0x3f) {
		DOCA_LOG_ERR("PSP crypt-offset must be a 6-bit integer");
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_config->net_config.crypt_offset = *int_param;
	DOCA_LOG_INFO("Configured PSP crypt_offset = %d", app_config->net_config.crypt_offset);

	return DOCA_SUCCESS;
}

/**
 * @brief Configures the PSP version to use for outgoing connections.
 *
 * @param [in]: A pointer to the parameter
 * @config [in/out]: A void pointer to the application config struct
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static doca_error_t handle_psp_version_param(void *param, void *config)
{
	auto *app_config = (struct psp_gw_app_config *)config;
	int *int_param = (int *)param;
	if (!SUPPORTED_PSP_VERSIONS.count(*int_param)) {
		DOCA_LOG_ERR("Unsupported PSP version: %d", *int_param);
		return DOCA_ERROR_INVALID_VALUE;
	}

	app_config->net_config.default_psp_proto_ver = *int_param;
	DOCA_LOG_INFO("Configured PSP version = %d", app_config->net_config.default_psp_proto_ver);

	return DOCA_SUCCESS;
}

/**
 * @brief Indicates the application should log all encryption keys
 *
 * @param [in]: A pointer to a boolean flag
 * @config [in/out]: A void pointer to the application config struct
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static doca_error_t handle_debug_keys_param(void *param, void *config)
{
	auto *app_config = (struct psp_gw_app_config *)config;
	bool *bool_param = (bool *)param;
	app_config->debug_keys = *bool_param;
	if (*bool_param) {
		DOCA_LOG_INFO("NOTE: debug_keys is enabled; crypto keys will be written to logs.");
	}
	return DOCA_SUCCESS;
}

/**
 * @brief Utility function to create a single argp parameter
 *
 * @short_name [in]: The single-letter command-line flag
 * @long_name [in]: The spelled-out command-line flag
 * @description [in]: Describes the option
 * @cb [in]: Called when the option is parsed
 * @arg_type [in]: How the option string should be parsed
 * @required [in]: Whether the program should terminate if the option is omitted
 * @accept_multiple [in]: Whether the program should accept multiple instances of the option
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static doca_error_t psp_gw_register_single_param(const char *short_name,
						 const char *long_name,
						 const char *description,
						 doca_argp_param_cb_t cb,
						 enum doca_argp_type arg_type,
						 bool required,
						 bool accept_multiple)
{
	struct doca_argp_param *param = NULL;
	doca_error_t result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	if (short_name)
		doca_argp_param_set_short_name(param, short_name);
	if (long_name)
		doca_argp_param_set_long_name(param, long_name);
	if (description)
		doca_argp_param_set_description(param, description);
	if (cb)
		doca_argp_param_set_callback(param, cb);
	if (required)
		doca_argp_param_set_mandatory(param);
	if (accept_multiple)
		doca_argp_param_set_multiplicity(param);

	doca_argp_param_set_type(param, arg_type);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param %s: %s",
			     long_name ? long_name : short_name,
			     doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/**
 * @brief Registers command-line arguments to the application.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t psp_gw_register_params(void)
{
	doca_error_t result;

	result = psp_gw_register_single_param("p",
					      "pci-addr",
					      "PCI BDF of the device in BB:DD.F format",
					      handle_pci_addr_param,
					      DOCA_ARGP_TYPE_STRING,
					      true,
					      false);
	if (result != DOCA_SUCCESS)
		return result;

	result = psp_gw_register_single_param("r",
					      "repr",
					      "Device representor list in vf[x-y]pf[x-y] format",
					      handle_repr_param,
					      DOCA_ARGP_TYPE_STRING,
					      true,
					      false);
	if (result != DOCA_SUCCESS)
		return result;

	result = psp_gw_register_single_param("m",
					      "core-mask",
					      "EAL Core Mask",
					      handle_core_mask_param,
					      DOCA_ARGP_TYPE_STRING,
					      false,
					      false);
	if (result != DOCA_SUCCESS)
		return result;

	result = psp_gw_register_single_param("d",
					      "decap-dmac",
					      "mac_dst addr of the decapped packets",
					      handle_decap_dmac_param,
					      DOCA_ARGP_TYPE_STRING,
					      true,
					      false);
	if (result != DOCA_SUCCESS)
		return result;

	result = psp_gw_register_single_param("n",
					      "nexthop-dmac",
					      "next-hop mac_dst addr of the encapped packets",
					      handle_nexthop_dmac_param,
					      DOCA_ARGP_TYPE_STRING,
					      false,
					      false);
	if (result != DOCA_SUCCESS)
		return result;

	result = psp_gw_register_single_param("s",
					      "svc-addr",
					      "Service address of locally running gRPC server; port number optional",
					      handle_svc_addr_param,
					      DOCA_ARGP_TYPE_STRING,
					      false,
					      false);
	if (result != DOCA_SUCCESS)
		return result;

	result = psp_gw_register_single_param("t",
					      "tunnel",
					      "Remote host tunnel(s), formatted 'mac-addr,phys-ip,virt-ip'",
					      handle_host_param,
					      DOCA_ARGP_TYPE_STRING,
					      false,
					      true);
	if (result != DOCA_SUCCESS)
		return result;

	result = psp_gw_register_single_param("f",
					      "tunnels-file",
					      "Specifies the location of the tunnels-file. "
					      "Format: rpc-addr:virt-addr,virt-addr,...",
					      handle_tunnels_file_param,
					      DOCA_ARGP_TYPE_STRING,
					      false,
					      true);
	if (result != DOCA_SUCCESS)
		return result;

	result = psp_gw_register_single_param("c",
					      "cookie",
					      "Enable use of PSP virtualization cookies",
					      handle_vc_param,
					      DOCA_ARGP_TYPE_BOOLEAN,
					      false,
					      false);
	if (result != DOCA_SUCCESS)
		return result;

	result = psp_gw_register_single_param("a",
					      "disable-ingress-acl",
					      "Allows any ingress packet that successfully decrypts",
					      handle_ingress_acl_param,
					      DOCA_ARGP_TYPE_BOOLEAN,
					      false,
					      false);
	if (result != DOCA_SUCCESS)
		return result;

	result = psp_gw_register_single_param("",
					      "sample-rate",
					      "Sets the log2 sample rate: 0: disabled, 1: 50%, ... 16: 1.5e-3%",
					      handle_sample_param,
					      DOCA_ARGP_TYPE_INT,
					      false,
					      false);
	if (result != DOCA_SUCCESS)
		return result;

	result = psp_gw_register_single_param("x",
					      "max-tunnels",
					      "Specify the max number of PSP tunnels",
					      handle_max_tunnels_param,
					      DOCA_ARGP_TYPE_INT,
					      false,
					      false);
	if (result != DOCA_SUCCESS)
		return result;

	result = psp_gw_register_single_param("o",
					      "crypt-offset",
					      "Specify the PSP crypt offset",
					      handle_psp_crypt_offset_param,
					      DOCA_ARGP_TYPE_INT,
					      false,
					      false);
	if (result != DOCA_SUCCESS)
		return result;

	result = psp_gw_register_single_param(nullptr,
					      "psp-version",
					      "Specify the PSP version for outgoing connections (0 or 1)",
					      handle_psp_version_param,
					      DOCA_ARGP_TYPE_INT,
					      false,
					      false);
	if (result != DOCA_SUCCESS)
		return result;

	result = psp_gw_register_single_param("z",
					      "static-tunnels",
					      "Create tunnels at startup using the given local IP addr",
					      handle_static_tunnels_param,
					      DOCA_ARGP_TYPE_STRING,
					      false,
					      false);
	if (result != DOCA_SUCCESS)
		return result;

	result = psp_gw_register_single_param("k",
					      "debug-keys",
					      "Enable debug keys",
					      handle_debug_keys_param,
					      DOCA_ARGP_TYPE_BOOLEAN,
					      false,
					      false);

	result = psp_gw_register_single_param("b",
					      "benchmark",
					      "Run PSP Benchmarks and exit",
					      handle_benchmark_param,
					      DOCA_ARGP_TYPE_BOOLEAN,
					      false,
					      false);
	if (result != DOCA_SUCCESS)
		return result;

	result = psp_gw_register_single_param(nullptr,
					      "spray",
					      "Spray packets across SPI tunnels",
					      handle_spray_param,
					      DOCA_ARGP_TYPE_BOOLEAN,
					      false,
					      false);
	if (result != DOCA_SUCCESS)
		return result;

	return result;
}

doca_error_t psp_gw_argp_exec(int &argc, char *argv[], psp_gw_app_config *app_config)
{
	doca_error_t result;

	// Init ARGP interface and start parsing cmdline/json arguments
	result = doca_argp_init("doca_psp_gateway", app_config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		return result;
	}

	result = psp_gw_register_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP parameters: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse application input: %s", doca_error_get_descr(result));
		doca_argp_destroy();
		return result;
	}

	const char *eal_args[] = {"", "-a00:00.0", "-c", app_config->core_mask.c_str()};
	int n_eal_args = sizeof(eal_args) / sizeof(eal_args[0]);
	int rc = rte_eal_init(n_eal_args, (char **)eal_args);
	if (rc < 0) {
		DOCA_LOG_ERR("EAL initialization failed");
		return DOCA_ERROR_DRIVER;
	}

	return DOCA_SUCCESS;
}
