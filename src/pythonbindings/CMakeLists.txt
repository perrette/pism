if (Pism_BUILD_PYTHON_BINDINGS)
    include(${SWIG_USE_FILE})

    include_directories (../verif/tests/ . ${PETSC4PY_INCLUDES} ${PYTHON_INCLUDES})
    
    set(PISM_PYTHON_BINDING_LIBRARIES "pismbase" "pismutil" "pismflowlaws" "pismverif" "pismstressbalance" )

    SET(CMAKE_SWIG_FLAGS "")
    SET(SWIG_MODULE_NAME "cpp")

    SET_SOURCE_FILES_PROPERTIES(PISM.i PROPERTIES CPLUSPLUS ON)

    set_source_files_properties(PISM.i PROPERTIES SWIG_MODULE_NAME cpp)

    SWIG_ADD_MODULE(${SWIG_MODULE_NAME} python PISM.i pism_python.cc pism_python_signal.cpp)
    SWIG_LINK_LIBRARIES(${SWIG_MODULE_NAME} ${PYTHON_LIBRARY} ${PETSC_LIBRARIES} ${MPI_LIBRARIES} ${PISM_PYTHON_BINDING_LIBRARIES} )

    get_target_property(SWIG_MODULE_LOCATION ${SWIG_MODULE_cpp_REAL_NAME} LOCATION)

    # Install Python .py files
    install(FILES
      ${SWIG_MODULE_LOCATION} ${CMAKE_CURRENT_BINARY_DIR}/${SWIG_MODULE_NAME}.py
      DESTINATION ${PISM_INSTALL_PYTHON_MODULE_DIR}/PISM
      COMPONENT RuntimeLibraries
    )

    install(DIRECTORY
      ${CMAKE_SOURCE_DIR}/site-packages/PISM
      DESTINATION ${PISM_INSTALL_PYTHON_MODULE_DIR}
      USE_SOURCE_PERMISSIONS
      COMPONENT RuntimeLibraries
    )
    
    
endif(Pism_BUILD_PYTHON_BINDINGS)