include_directories (../../verif/tests/ ../util)

add_library (pismstressbalance
  PISMStressBalance.cc
  PISMStressBalance_diagnostics.cc
  ShallowStressBalance.cc
  SSB_Modifier.cc
  PISMBedSmoother.cc
  SIAFD.cc
  SIAFD_diagnostics.cc
  SSA.cc
  SSA_diagnostics.cc
  SSAFD.cc
  SSAFEM.cc  
  SSAFEM_Forward.cc  
  FETools.cc
  SSATestCase.cc
  SIA_Sliding.cc
)
target_link_libraries (pismstressbalance pismutil pismflowlaws)

install (TARGETS pismstressbalance DESTINATION ${Pism_LIB_DIR})

if (Pism_BUILD_EXTRA_EXECS)
  add_executable (siafd_test siafd_test.cc ../../verif/tests/exactTestsFG.c ../basal_strength/basal_resistance.cc)
  target_link_libraries (siafd_test pismstressbalance)

  install (TARGETS siafd_test DESTINATION ${Pism_BIN_DIR})
endif ()

add_subdirectory (ssa_tests)