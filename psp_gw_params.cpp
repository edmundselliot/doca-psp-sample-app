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

#include <psp_gw_config.h>
#include <psp_gw_params.h>

DOCA_LOG_REGISTER(PSP_Gateway_Params);

/**
 * @brief Specifies the PF to be used by the host
 *
 * @param [in]: A string of the form DDDD:BB:DD.F describing the PCI DBDF
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
 * @brief Indicates which representor[s] of the PF should be probed
 *
 * @param [in]: A string of the form xf#, where:
 *              # is a number, or a range in square brackets
 *              xf is one of: vf, sf, pf (optional)
 * @config [in/out]: A void pointer to the application config struct
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static doca_error_t handle_repr_param(void *param, void *config)
{
	auto *app_config = (struct psp_gw_app_config *)config;
	app_config->pf_repr_indices = (const char *)param;
	return DOCA_SUCCESS;
}

/**
 * @brief Parses a tunnel specifier for a remote host
 *
 * @fields [in]: A comma-separated string containing the following:
 * - rpc_ipv4_addr
 * - mac_addr
 * - outer_ipv6_addr
 * - inner_ipv4_addr
 * @host [out]: The host data structure to populate
 * @return: DOCA_SUCCESS on success; DOCA_ERROR otherwise
 */
static bool parse_host_param(char *fields, struct psp_gw_host *host)
{
	char *svcaddr = strtok_r(fields, ",", &fields);
	char *macaddr = strtok_r(fields, ",", &fields);
	char *phys_ip = strtok_r(fields, ",", &fields);
	char *virt_ip = strtok_r(fields, ",", &fields);
	char *extra_field_check = strtok_r(fields, ",", &fields); // expect null

	if (!svcaddr || !macaddr || !phys_ip || !virt_ip || extra_field_check) {
		DOCA_LOG_ERR("Tunnel host requires 4 args: svc_ip,mac,pip,vip");
		return false;
	}

	if (inet_pton(AF_INET, svcaddr, &host->svc_ip) != 1) {
		DOCA_LOG_ERR("Invalid svc IPv4 addr: %s", svcaddr);
		return false;
	}
	if (rte_ether_unformat_addr(macaddr, &host->mac) != 0) {
		DOCA_LOG_ERR("Invalid mac addr: %s", macaddr);
		return false;
	}
	if (inet_pton(AF_INET6, phys_ip, host->pip) != 1) {
		DOCA_LOG_ERR("Invalid physical IPv6 addr: %s", phys_ip);
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

doca_error_t psp_gw_register_params(void)
{
	doca_error_t result;

	result = psp_gw_register_single_param("p",
					      "pci-addr",
					      "PCI address of Physical Function",
					      handle_pci_addr_param,
					      DOCA_ARGP_TYPE_STRING,
					      true,
					      false);

	result = psp_gw_register_single_param("r",
					      "representors",
					      "Representor indices to attach; supported format: '#', '#,#', '#-#'",
					      handle_repr_param,
					      DOCA_ARGP_TYPE_STRING,
					      false,
					      false);

	result = psp_gw_register_single_param("s",
					      "svc-addr",
					      "Service address of locally running gRPC server; port number optional",
					      handle_svc_addr_param,
					      DOCA_ARGP_TYPE_STRING,
					      false,
					      false);

	result = psp_gw_register_single_param("t",
					      "tunnel",
					      "Remote host tunnel(s), formatted 'mac-addr,phys-ip,virt-ip'",
					      handle_host_param,
					      DOCA_ARGP_TYPE_STRING,
					      false,
					      true);

	result = psp_gw_register_single_param("c",
					      "cookie",
					      "Enable use of PSP virtualization cookies",
					      handle_vc_param,
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

	return DOCA_SUCCESS;
}
