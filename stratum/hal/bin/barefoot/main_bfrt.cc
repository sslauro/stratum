// Copyright 2018-present Barefoot Networks, Inc.
// SPDX-License-Identifier: Apache-2.0

extern "C" {

#include "bf_switchd/bf_switchd.h"

int switch_pci_sysfs_str_get(char* name, size_t name_size);
}

#include "gflags/gflags.h"
#include "stratum/glue/init_google.h"
#include "stratum/glue/logging.h"
#include "stratum/hal/lib/barefoot/bf_chassis_manager.h"
#include "stratum/hal/lib/barefoot/bf_pal_wrapper.h"
#include "stratum/hal/lib/barefoot/bf_pd_wrapper.h"
#include "stratum/hal/lib/barefoot/bf_switch_bfrt.h"
#include "stratum/hal/lib/barefoot/bfrt_node.h"
#include "stratum/hal/lib/common/hal.h"
#include "stratum/hal/lib/phal/phal.h"
#include "stratum/hal/lib/phal/phal_sim.h"
#include "stratum/lib/security/auth_policy_checker.h"
#include "stratum/lib/security/credentials_manager.h"

DEFINE_string(bf_sde_install, "",
              "Absolute path to the directory where the BF SDE is installed");
DEFINE_bool(bf_switchd_background, false,
            "Run bf_switchd in the background with no interactive features");
DEFINE_string(bf_switchd_cfg, "stratum/hal/bin/barefoot/tofino_skip_p4.conf",
              "Path to the BF switchd json config file");
DEFINE_bool(bf_sim, false, "Run with the Tofino simulator");

namespace stratum {
namespace hal {
namespace barefoot {

::util::Status Main(int argc, char* argv[]) {
  InitGoogle(argv[0], &argc, &argv, true);
  InitStratumLogging();

  char bf_sysfs_fname[128];
  FILE* fd;

  auto switchd_main_ctx = absl::make_unique<bf_switchd_context_t>();

  /* Parse bf_switchd arguments */
  CHECK_RETURN_IF_FALSE(FLAGS_bf_sde_install != "")
      << "Flag --bf_sde_install is required";
  switchd_main_ctx->install_dir = strdup(FLAGS_bf_sde_install.c_str());
  switchd_main_ctx->conf_file = strdup(FLAGS_bf_switchd_cfg.c_str());
  switchd_main_ctx->skip_p4 = true;
  if (FLAGS_bf_switchd_background)
    switchd_main_ctx->running_in_background = true;
  else
    switchd_main_ctx->shell_set_ucli = true;

  /* determine if kernel mode packet driver is loaded */
  switch_pci_sysfs_str_get(bf_sysfs_fname,
                           sizeof(bf_sysfs_fname) - sizeof("/dev_add"));
  strncat(bf_sysfs_fname, "/dev_add", sizeof("/dev_add"));
  printf("bf_sysfs_fname %s\n", bf_sysfs_fname);
  fd = fopen(bf_sysfs_fname, "r");
  if (fd != NULL) {
    /* override previous parsing if bf_kpkt KLM was loaded */
    printf("kernel mode packet driver present, forcing kernel_pkt option!\n");
    switchd_main_ctx->kernel_pkt = true;
    fclose(fd);
  }

  {
    int status = bf_switchd_lib_init(switchd_main_ctx.get());
    CHECK_RETURN_IF_FALSE(status == 0)
        << "Error when starting switchd, status: " << status;
    LOG(INFO) << "switchd started successfully";
  }

  int unit(0);
  // TODO(antonin): The SDE expects 0-based device ids, so we instantiate
  // DeviceMgr with "unit" instead of "node_id". This works because DeviceMgr
  // does not do any device id checks.

  auto bfrt_id_mapper = BfRtIdMapper::CreateInstance(unit);
  auto bfrt_table_manager =
      BfRtTableManager::CreateInstance(unit, bfrt_id_mapper.get());
  auto& bf_device_manager = bfrt::BfRtDevMgr::getInstance();
  auto bfrt_node = BfRtNode::CreateInstance(
      bfrt_table_manager.get(), &bf_device_manager, bfrt_id_mapper.get(), unit);
  PhalInterface* phal_impl;
  if (FLAGS_bf_sim) {
    phal_impl = PhalSim::CreateSingleton();
  } else {
    phal_impl = phal::Phal::CreateSingleton();
  }

  std::map<int, BfRtNode*> unit_to_bfrt_node = {
      {unit, bfrt_node.get()},
  };
  auto bf_chassis_manager =
      BFChassisManager::CreateInstance(phal_impl, BFPalWrapper::GetSingleton());
  auto bfpd_wrapper = BFPdWrapper::GetSingleton();
  auto bf_switch =
      BFSwitch::CreateInstance(phal_impl, bf_chassis_manager.get(),
                               bfpd_wrapper, unit_to_bfrt_node);

  // Create the 'Hal' class instance.
  auto auth_policy_checker = AuthPolicyChecker::CreateInstance();
  ASSIGN_OR_RETURN(auto credentials_manager,
                   CredentialsManager::CreateInstance());
  auto* hal = Hal::CreateSingleton(stratum::hal::OPERATION_MODE_STANDALONE,
                                   bf_switch.get(), auth_policy_checker.get(),
                                   credentials_manager.get());
  CHECK_RETURN_IF_FALSE(hal) << "Failed to create the Stratum Hal instance.";

  // Setup and start serving RPCs.
  ::util::Status status = hal->Setup();
  if (!status.ok()) {
    LOG(ERROR)
        << "Error when setting up Stratum HAL (but we will continue running): "
        << status.error_message();
  }

  RETURN_IF_ERROR(hal->Run());  // blocking
  LOG(INFO) << "See you later!";
  return ::util::OkStatus();
}

}  // namespace barefoot
}  // namespace hal
}  // namespace stratum

int main(int argc, char* argv[]) {
  return stratum::hal::barefoot::Main(argc, argv).error_code();
}