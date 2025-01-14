# ***** BEGIN APACHE 2 LICENSE BLOCK *****
#
# Copyright 2019 Elie Michel
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# ***** END APACHE 2 LICENSE BLOCK *****

if(WITH_GTESTS)
  Include(GTestTesting)
  remove_strict_flags()

  add_definitions(${GFLAGS_DEFINES})
  add_definitions(${GLOG_DEFINES})
  add_definitions(-DBLENDER_GFLAGS_NAMESPACE=${GFLAGS_NAMESPACE})

  set(SRC
    test_plugin_load.cpp
  )

  set(INC
    .
  )

  set(LIB
    OpenMfx
    OpenMfx_Host
    OpenMfx_Utils
  )

  if (MSVC)
    add_definitions(-DFULL_LIBRARY_OUTPUT_PATH="${LIBRARY_OUTPUT_PATH}/$<CONFIG>/")
  else(MSVC)
    add_definitions(-DFULL_LIBRARY_OUTPUT_PATH="${LIBRARY_OUTPUT_PATH}/")
  endif(MSVC)

  BLENDER_SRC_GTEST("openmfx_plugin_load" "${SRC}" "${LIB}")
  target_include_directories(openmfx_plugin_load_test PRIVATE ${INC})
  set_property(TARGET openmfx_plugin_load_test PROPERTY FOLDER "OpenMfx")
endif()
