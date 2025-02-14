/*
 * Copyright © 2020-2021 Inria.  All rights reserved.
 * See COPYING in top-level directory.
 */

#include "private/autogen/config.h"
#include "hwloc.h"
#include "hwloc/plugins.h"

/* private headers allowed for convenience because this plugin is built within hwloc */
#include "private/misc.h"
#include "private/debug.h"

#include <level_zero/ze_api.h>
#include <level_zero/zes_api.h>

static void
hwloc__levelzero_properties_get(ze_device_handle_t h, hwloc_obj_t osdev,
                                int sysman_maybe_missing,
                                int *is_integrated_p)
{
  ze_result_t res;
  ze_device_properties_t prop;
  zes_device_properties_t prop2;
  int is_subdevice = 0;
  int is_integrated = 0;

  memset(&prop, 0, sizeof(prop));
  res = zeDeviceGetProperties(h, &prop);
  if (res == ZE_RESULT_SUCCESS) {
    /* name is the model name followed by the deviceID
     * flags 1<<0 means integrated (vs discrete).
     */
    char tmp[64];
    const char *type;
    switch (prop.type) {
    case ZE_DEVICE_TYPE_GPU: type = "GPU"; break;
    case ZE_DEVICE_TYPE_CPU: type = "CPU"; break;
    case ZE_DEVICE_TYPE_FPGA: type = "FPGA"; break;
    case ZE_DEVICE_TYPE_MCA: type = "MCA"; break;
    case ZE_DEVICE_TYPE_VPU: type = "VPU"; break;
    default:
      if (!hwloc_hide_errors())
        fprintf(stderr, "hwloc/levelzero: unexpected device type %u\n", (unsigned) prop.type);
      type = "Unknown";
    }
    hwloc_obj_add_info(osdev, "LevelZeroDeviceType", type);
    snprintf(tmp, sizeof(tmp), "%u", prop.numSlices);
    hwloc_obj_add_info(osdev, "LevelZeroNumSlices", tmp);
    snprintf(tmp, sizeof(tmp), "%u", prop.numSubslicesPerSlice);
    hwloc_obj_add_info(osdev, "LevelZeroNumSubslicesPerSlice", tmp);
    snprintf(tmp, sizeof(tmp), "%u", prop.numEUsPerSubslice);
    hwloc_obj_add_info(osdev, "LevelZeroNumEUsPerSubslice", tmp);
    snprintf(tmp, sizeof(tmp), "%u", prop.numThreadsPerEU);
    hwloc_obj_add_info(osdev, "LevelZeroNumThreadsPerEU", tmp);

    if (prop.flags & ZE_DEVICE_PROPERTY_FLAG_SUBDEVICE)
      is_subdevice = 1;

    if (prop.flags & ZE_DEVICE_PROPERTY_FLAG_INTEGRATED)
      is_integrated = 1;
  }

  if (is_integrated_p)
    *is_integrated_p = is_integrated;

  if (is_subdevice)
    /* sysman API on subdevice returns the same as root device, and we don't need those duplicate attributes */
    return;

  /* try to get additional info from sysman if enabled */
  memset(&prop2, 0, sizeof(prop2));
  res = zesDeviceGetProperties(h, &prop2);
  if (res == ZE_RESULT_SUCCESS) {
    /* old implementations may return "Unknown", recent may return "unknown" */
    if (strcasecmp((const char *) prop2.vendorName, "Unknown"))
      hwloc_obj_add_info(osdev, "LevelZeroVendor", (const char *) prop2.vendorName);
    if (strcasecmp((const char *) prop2.modelName, "Unknown"))
      hwloc_obj_add_info(osdev, "LevelZeroModel", (const char *) prop2.modelName);
    if (strcasecmp((const char *) prop2.brandName, "Unknown"))
      hwloc_obj_add_info(osdev, "LevelZeroBrand", (const char *) prop2.brandName);
    if (strcasecmp((const char *) prop2.serialNumber, "Unknown"))
      hwloc_obj_add_info(osdev, "LevelZeroSerialNumber", (const char *) prop2.serialNumber);
    if (strcasecmp((const char *) prop2.boardNumber, "Unknown"))
      hwloc_obj_add_info(osdev, "LevelZeroBoardNumber", (const char *) prop2.boardNumber);
  } else {
    static int warned = 0;
    if (!warned) {
      if (sysman_maybe_missing == 1 && !hwloc_hide_errors())
        fprintf(stderr, "hwloc/levelzero: zesDeviceGetProperties() failed (ZES_ENABLE_SYSMAN=1 set too late?).\n");
      else if (sysman_maybe_missing == 2 && !hwloc_hide_errors())
        fprintf(stderr, "hwloc/levelzero: zesDeviceGetProperties() failed (ZES_ENABLE_SYSMAN=0).\n");
      warned = 1;
    }
    /* continue in degraded mode, we'll miss locality and some attributes */
  }
}

static void
hwloc__levelzero_cqprops_get(ze_device_handle_t h,
                             hwloc_obj_t osdev)
{
  ze_command_queue_group_properties_t *cqprops;
  unsigned nr_cqprops = 0;
  ze_result_t res;

  res = zeDeviceGetCommandQueueGroupProperties(h, &nr_cqprops, NULL);
  if (res != ZE_RESULT_SUCCESS || !nr_cqprops)
    return;

  cqprops = malloc(nr_cqprops * sizeof(*cqprops));
  if (cqprops) {
    res = zeDeviceGetCommandQueueGroupProperties(h, &nr_cqprops, cqprops);
    if (res == ZE_RESULT_SUCCESS) {
      unsigned k;
      char tmp[32];
      snprintf(tmp, sizeof(tmp), "%u", nr_cqprops);
      hwloc_obj_add_info(osdev, "LevelZeroCQGroups", tmp);
      for(k=0; k<nr_cqprops; k++) {
        char name[32];
        snprintf(name, sizeof(name), "LevelZeroCQGroup%u", k);
        snprintf(tmp, sizeof(tmp), "%u*0x%lx", (unsigned) cqprops[k].numQueues, (unsigned long) cqprops[k].flags);
        hwloc_obj_add_info(osdev, name, tmp);
      }
    }
    free(cqprops);
  }
}

static int
hwloc__levelzero_memory_get_from_sysman(zes_device_handle_t h,
                                        hwloc_obj_t root_osdev,
                                        unsigned nr_osdevs, hwloc_obj_t *sub_osdevs)
{
  zes_mem_handle_t *mh;
  uint32_t nr_mems;
  ze_result_t res;
  unsigned long long totalHBMkB = 0;
  unsigned long long totalDDRkB = 0;

  nr_mems = 0;
  res = zesDeviceEnumMemoryModules(h, &nr_mems, NULL);
  if (res != ZE_RESULT_SUCCESS)
    return -1; /* notify that sysman failed */

  hwloc_debug("L0/Sysman: found %u memory modules in osdev %s\n",
              nr_mems, root_osdev->name);
  if (!nr_mems)
    return 0;

  mh = malloc(nr_mems * sizeof(*mh));
  if (mh) {
    res = zesDeviceEnumMemoryModules(h, &nr_mems, mh);
    if (res == ZE_RESULT_SUCCESS) {
      unsigned m;
      for(m=0; m<nr_mems; m++) {
        zes_mem_properties_t mprop;
        res = zesMemoryGetProperties(mh[m], &mprop);
        if (res == ZE_RESULT_SUCCESS) {
          const char *type;
          hwloc_obj_t osdev;
          char name[ZE_MAX_DEVICE_NAME+64], value[64];

          if (!mprop.physicalSize) {
            /* unknown, but memory state should have it */
            zes_mem_state_t s;
            res = zesMemoryGetState(mh[m], &s);
            if (res == ZE_RESULT_SUCCESS) {
              hwloc_debug("L0/Sysman: found size 0 for memory modules #%u, using memory state size instead\n", m);
              mprop.physicalSize = s.size;
            }
          }

          if (mprop.onSubdevice) {
            if (mprop.subdeviceId >= nr_osdevs || !nr_osdevs || !sub_osdevs) {
              if (!hwloc_hide_errors())
                fprintf(stderr, "LevelZero: memory module #%u on unexpected subdeviceId #%u\n", m, mprop.subdeviceId);
              osdev = NULL; /* we'll ignore it but we'll still agregate its subdevice memories into totalHBM/DDRkB */
            } else {
              osdev = sub_osdevs[mprop.subdeviceId];
            }
          } else {
            osdev = root_osdev;
          }
          switch (mprop.type) {
          case ZES_MEM_TYPE_HBM:
            type = "HBM";
            totalHBMkB += mprop.physicalSize >> 10;
            break;
          case ZES_MEM_TYPE_DDR:
          case ZES_MEM_TYPE_DDR3:
          case ZES_MEM_TYPE_DDR4:
          case ZES_MEM_TYPE_DDR5:
          case ZES_MEM_TYPE_LPDDR:
          case ZES_MEM_TYPE_LPDDR3:
          case ZES_MEM_TYPE_LPDDR4:
          case ZES_MEM_TYPE_LPDDR5:
            type = "DDR";
            totalDDRkB += mprop.physicalSize >> 10;
            break;
          default:
            type = "Memory";
          }

          hwloc_debug("L0/Sysman: found %llu bytes type %s for osdev %s (onsub %d subid %u)\n",
                      (unsigned long long) mprop.physicalSize, type, osdev ? osdev->name : "NULL",
                      mprop.onSubdevice, mprop.subdeviceId);
          if (!osdev || !type || !mprop.physicalSize)
            continue;

          if (osdev != root_osdev) {
            /* set the subdevice memory immediately */
            snprintf(name, sizeof(name), "LevelZero%sSize", type);
            snprintf(value, sizeof(value), "%llu", (unsigned long long) mprop.physicalSize >> 10);
            hwloc_obj_add_info(osdev, name, value);
          }
        }
      }
    }
    free(mh);
  }

  /* set the root device memory at the end, once subdevice memories were agregated */
  if (totalHBMkB) {
    char value[64];
    snprintf(value, sizeof(value), "%llu", totalHBMkB);
    hwloc_obj_add_info(root_osdev, "LevelZeroHBMSize", value);
  }
  if (totalDDRkB) {
    char value[64];
    snprintf(value, sizeof(value), "%llu", totalDDRkB);
    hwloc_obj_add_info(root_osdev, "LevelZeroDDRSize", value);
  }

  return 0;
}

static void
hwloc__levelzero_memory_get_from_coreapi(ze_device_handle_t h,
                                         hwloc_obj_t osdev,
                                         int ignore_ddr)
{
  ze_device_memory_properties_t *mh;
  uint32_t nr_mems;
  ze_result_t res;

  nr_mems = 0;
  res = zeDeviceGetMemoryProperties(h, &nr_mems, NULL);
  if (res != ZE_RESULT_SUCCESS || !nr_mems)
    return;
  hwloc_debug("L0/CoreAPI: found %u memories in osdev %s\n",
              nr_mems, osdev->name);

  mh = malloc(nr_mems * sizeof(*mh));
  if (mh) {
    res = zeDeviceGetMemoryProperties(h, &nr_mems, mh);
    if (res == ZE_RESULT_SUCCESS) {
      unsigned m;
      for(m=0; m<nr_mems; m++) {
        const char *_name = mh[m].name;
        char name[300], value[64];
        /* FIXME: discrete GPUs report 95% of the physical memory (what sysman sees)
         * while integrated GPUs report 80% of the host RAM (sysman sees 0), adjust?
         */
        hwloc_debug("L0/CoreAPI: found memory name %s size %llu in osdev %s\n",
                    mh[m].name, (unsigned long long) mh[m].totalSize, osdev->name);
        if (!mh[m].totalSize)
          continue;
        if (ignore_ddr && !strcmp(_name, "DDR"))
          continue;
        if (!_name[0])
          _name = "Memory";
        snprintf(name, sizeof(name), "LevelZero%sSize", _name); /* HBM or DDR, or Memory if unknown */
        snprintf(value, sizeof(value), "%llu", (unsigned long long) mh[m].totalSize >> 10);
        hwloc_obj_add_info(osdev, name, value);
      }
    }
    free(mh);
  }
}


static void
hwloc__levelzero_memory_get(zes_device_handle_t h, hwloc_obj_t root_osdev, int is_integrated,
                            unsigned nr_subdevices, zes_device_handle_t *subh, hwloc_obj_t *sub_osdevs)
{
  static int memory_from_coreapi = -1; /* 1 means coreapi, 0 means sysman, -1 means sysman if available or coreapi otherwise */
  static int first = 1;

  if (first) {
    char *env;
    env = getenv("HWLOC_L0_COREAPI_MEMORY");
    if (env)
      memory_from_coreapi = atoi(env);

    if (memory_from_coreapi == -1) {
      int ret = hwloc__levelzero_memory_get_from_sysman(h, root_osdev, nr_subdevices, sub_osdevs);
      if (!ret) {
        /* sysman worked, we're done, disable coreapi for next time */
        hwloc_debug("levelzero: sysman/memory succeeded, disabling coreapi memory queries\n");
        memory_from_coreapi = 0;
        return;
      }
      /* sysman failed, enable coreapi */
      hwloc_debug("levelzero: sysman/memory failed, enabling coreapi memory queries\n");
      memory_from_coreapi = 1;
    }

    first = 0;
  }

  if (memory_from_coreapi > 0) {
    unsigned k;
    int ignore_ddr = (memory_from_coreapi != 2) && is_integrated; /* DDR ignored in integrated GPUs, it's like the host DRAM */
    hwloc__levelzero_memory_get_from_coreapi(h, root_osdev, ignore_ddr);
    for(k=0; k<nr_subdevices; k++)
      hwloc__levelzero_memory_get_from_coreapi(subh[k], sub_osdevs[k], ignore_ddr);
  } else {
    hwloc__levelzero_memory_get_from_sysman(h, root_osdev, nr_subdevices, sub_osdevs);
    /* no need to call hwloc__levelzero_memory_get() on subdevices,
     * the call on the root device is enough (and identical to a call on subdevices)
     */
  }
}

static int
hwloc_levelzero_discover(struct hwloc_backend *backend, struct hwloc_disc_status *dstatus)
{
  /*
   * This backend uses the underlying OS.
   * However we don't enforce topology->is_thissystem so that
   * we may still force use this backend when debugging with !thissystem.
   */

  struct hwloc_topology *topology = backend->topology;
  enum hwloc_type_filter_e filter;
  ze_result_t res;
  ze_driver_handle_t *drh;
  uint32_t nbdrivers, i, k, zeidx;
  int sysman_maybe_missing = 0; /* 1 if ZES_ENABLE_SYSMAN=1 was NOT set early, 2 if ZES_ENABLE_SYSMAN=0 */
  char *env;

  assert(dstatus->phase == HWLOC_DISC_PHASE_IO);

  hwloc_topology_get_type_filter(topology, HWLOC_OBJ_OS_DEVICE, &filter);
  if (filter == HWLOC_TYPE_FILTER_KEEP_NONE)
    return 0;

  /* Tell L0 to create sysman devices.
   * If somebody already initialized L0 without Sysman,
   * zesDeviceGetProperties() will fail and warn in hwloc__levelzero_properties_get().
   * The lib constructor and Windows DllMain tried to set ZES_ENABLE_SYSMAN=1 early (see topology.c),
   * we try again in case they didn't.
   */
  env = getenv("ZES_ENABLE_SYSMAN");
  if (!env) {
    putenv((char *) "ZES_ENABLE_SYSMAN=1");
    /* we'll warn in hwloc__levelzero_properties_get() if we fail to get zes devices */
    sysman_maybe_missing = 1;
  } else if (!atoi(env)) {
    sysman_maybe_missing = 2;
  }

  res = zeInit(0);
  if (res != ZE_RESULT_SUCCESS) {
    if (!hwloc_hide_errors()) {
      fprintf(stderr, "Failed to initialize LevelZero in ze_init(): %d\n", (int)res);
    }
    return 0;
  }

  nbdrivers = 0;
  res = zeDriverGet(&nbdrivers, NULL);
  if (res != ZE_RESULT_SUCCESS || !nbdrivers)
    return 0;
  drh = malloc(nbdrivers * sizeof(*drh));
  if (!drh)
    return 0;
  res = zeDriverGet(&nbdrivers, drh);
  if (res != ZE_RESULT_SUCCESS) {
    free(drh);
    return 0;
  }

  zeidx = 0;
  for(i=0; i<nbdrivers; i++) {
    uint32_t nbdevices, j;
    ze_device_handle_t *dvh;
    char buffer[13];

    nbdevices = 0;
    res = zeDeviceGet(drh[i], &nbdevices, NULL);
    if (res != ZE_RESULT_SUCCESS || !nbdevices)
      continue;
    dvh = malloc(nbdevices * sizeof(*dvh));
    if (!dvh)
      continue;
    res = zeDeviceGet(drh[i], &nbdevices, dvh);
    if (res != ZE_RESULT_SUCCESS) {
      free(dvh);
      continue;
    }

    for(j=0; j<nbdevices; j++) {
      zes_pci_properties_t pci;
      zes_device_handle_t sdvh = dvh[j];
      zes_device_handle_t *subh = NULL;
      uint32_t nr_subdevices;
      hwloc_obj_t osdev, parent, *subosdevs = NULL;
      int is_integrated = 0;

      osdev = hwloc_alloc_setup_object(topology, HWLOC_OBJ_OS_DEVICE, HWLOC_UNKNOWN_INDEX);
      snprintf(buffer, sizeof(buffer), "ze%u", zeidx); // ze0d0 ?
      osdev->name = strdup(buffer);
      osdev->depth = HWLOC_TYPE_DEPTH_UNKNOWN;
      osdev->attr->osdev.type = HWLOC_OBJ_OSDEV_COPROC;
      osdev->subtype = strdup("LevelZero");
      hwloc_obj_add_info(osdev, "Backend", "LevelZero");

      snprintf(buffer, sizeof(buffer), "%u", i);
      hwloc_obj_add_info(osdev, "LevelZeroDriverIndex", buffer);
      snprintf(buffer, sizeof(buffer), "%u", j);
      hwloc_obj_add_info(osdev, "LevelZeroDriverDeviceIndex", buffer);

      hwloc__levelzero_properties_get(dvh[j], osdev, sysman_maybe_missing, &is_integrated);

      hwloc__levelzero_cqprops_get(dvh[j], osdev);

      nr_subdevices = 0;
      res = zeDeviceGetSubDevices(dvh[j], &nr_subdevices, NULL);
      /* returns ZE_RESULT_ERROR_INVALID_ARGUMENT if there are no subdevices */
      if (res == ZE_RESULT_SUCCESS && nr_subdevices > 0) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%u", nr_subdevices);
        hwloc_obj_add_info(osdev, "LevelZeroSubdevices", tmp);
        subh = malloc(nr_subdevices * sizeof(*subh));
        subosdevs = malloc(nr_subdevices * sizeof(*subosdevs));
        if (subosdevs && subh) {
          zeDeviceGetSubDevices(dvh[j], &nr_subdevices, subh);
          for(k=0; k<nr_subdevices; k++) {
            subosdevs[k] = hwloc_alloc_setup_object(topology, HWLOC_OBJ_OS_DEVICE, HWLOC_UNKNOWN_INDEX);
            snprintf(buffer, sizeof(buffer), "ze%u.%u", zeidx, k);
            subosdevs[k]->name = strdup(buffer);
            subosdevs[k]->depth = HWLOC_TYPE_DEPTH_UNKNOWN;
            subosdevs[k]->attr->osdev.type = HWLOC_OBJ_OSDEV_COPROC;
            subosdevs[k]->subtype = strdup("LevelZero");
            hwloc_obj_add_info(subosdevs[k], "Backend", "LevelZero");
            snprintf(tmp, sizeof(tmp), "%u", k);
            hwloc_obj_add_info(subosdevs[k], "LevelZeroSubdeviceID", tmp);

            hwloc__levelzero_properties_get(subh[j], subosdevs[k], sysman_maybe_missing, NULL);

            hwloc__levelzero_cqprops_get(subh[k], subosdevs[k]);
          }
        } else {
          free(subosdevs);
          free(subh);
          subosdevs = NULL;
          nr_subdevices = 0;
        }
      }

      /* get all memory info at once */
      hwloc__levelzero_memory_get(dvh[j], osdev, is_integrated, nr_subdevices, subh, subosdevs);

      parent = NULL;
      res = zesDevicePciGetProperties(sdvh, &pci);
      if (res == ZE_RESULT_SUCCESS) {
        parent = hwloc_pci_find_parent_by_busid(topology,
                                                pci.address.domain,
                                                pci.address.bus,
                                                pci.address.device,
                                                pci.address.function);
        if (parent && parent->type == HWLOC_OBJ_PCI_DEVICE) {
          if (pci.maxSpeed.maxBandwidth > 0)
            parent->attr->pcidev.linkspeed = ((float)pci.maxSpeed.maxBandwidth)/1000/1000/1000;
        }
      }
      if (!parent)
        parent = hwloc_get_root_obj(topology);

      hwloc_insert_object_by_parent(topology, parent, osdev);
      if (nr_subdevices) {
        for(k=0; k<nr_subdevices; k++)
          if (subosdevs[k])
            hwloc_insert_object_by_parent(topology, osdev, subosdevs[k]);
        free(subosdevs);
        free(subh);
      }
      zeidx++;
    }

    free(dvh);
  }

  free(drh);
  return 0;
}

static struct hwloc_backend *
hwloc_levelzero_component_instantiate(struct hwloc_topology *topology,
                                      struct hwloc_disc_component *component,
                                      unsigned excluded_phases __hwloc_attribute_unused,
                                      const void *_data1 __hwloc_attribute_unused,
                                      const void *_data2 __hwloc_attribute_unused,
                                      const void *_data3 __hwloc_attribute_unused)
{
  struct hwloc_backend *backend;

  backend = hwloc_backend_alloc(topology, component);
  if (!backend)
    return NULL;
  backend->discover = hwloc_levelzero_discover;
  return backend;
}

static struct hwloc_disc_component hwloc_levelzero_disc_component = {
  "levelzero",
  HWLOC_DISC_PHASE_IO,
  HWLOC_DISC_PHASE_GLOBAL,
  hwloc_levelzero_component_instantiate,
  10, /* after pci */
  1,
  NULL
};

static int
hwloc_levelzero_component_init(unsigned long flags)
{
  if (flags)
    return -1;
  if (hwloc_plugin_check_namespace("levelzero", "hwloc_backend_alloc") < 0)
    return -1;
  return 0;
}

#ifdef HWLOC_INSIDE_PLUGIN
HWLOC_DECLSPEC extern const struct hwloc_component hwloc_levelzero_component;
#endif

const struct hwloc_component hwloc_levelzero_component = {
  HWLOC_COMPONENT_ABI,
  hwloc_levelzero_component_init, NULL,
  HWLOC_COMPONENT_TYPE_DISC,
  0,
  &hwloc_levelzero_disc_component
};
