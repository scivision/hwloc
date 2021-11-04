foreach(t api_version backends bind
  bitmap bitmap_compare_inclusion bitmap_first_last_weight bitmap_singlify bitmap_string
  distances get_area_memlocation get_cache_covering_cpuset get_closest_objs get_largest_objs_inside_cpuset
  get_last_cpu_location get_next_obj_covering_cpuset get_obj_below_array_by_type get_obj_covering_cpuset
  get_obj_inside_cpuset get_obj_with_same_locality get_shared_cache_covering_obj
  groups insert_misc iodevs is_thissystem list_components obj_infos object_userdata pci_backend synthetic
  topology_abi topology_allow topology_diff topology_dup topology_restrict type_depth type_sscanf
)
  add_executable(hwloc_${t} hwloc_${t}.c)
  target_link_libraries(hwloc_${t} PRIVATE hwloc::hwloc)

  add_test(NAME ${t} COMMAND hwloc_${t})
  set_tests_properties(${t} PROPERTIES TIMEOUT 10)
endforeach()

target_compile_definitions(hwloc_type_sscanf PRIVATE XMLTESTDIR=\"${CMAKE_CURRENT_SOURCE_DIR}/xml/\")

set_tests_properties(get_obj_with_same_locality PROPERTIES ENVIRONMENT HWLOC_TOP_SRCDIR=${TOPDIR})

# --- optional tests

find_package(OpenCL)
if(OpenCL_FOUND)
  add_executable(test_opencl opencl.c)
  target_link_libraries(test_opencl PRIVATE hwloc::hwloc OpenCL::OpenCL)

  add_test(NAME OpenCL COMMAND test_opencl)
  set_tests_properties(OpenCL PROPERTIES TIMEOUT 10)
endif()
