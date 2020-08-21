# see also `make docs' -- alternative variant with less dependencies

include(FindDoxygen)

if(NOT DOXYGEN_FOUND)
  message(WARNING "Doxygen not found.")
else()
  set(DOXY_CONTRACTS_VERSION "${CONTRACTS_VER_DIRTY}"           CACHE INTERNAL "Version string used in PROJECT_NUMBER.")
  # trailing separators ('/') are added in the Doxyfile
  set(DOXY_DOC_DEST_DIR "${CMAKE_BINARY_DIR}/docs/doxygen" CACHE PATH "Path to the doxygen output")
  set(DOXY_DOC_INPUT_DIRS "${CMAKE_SOURCE_DIR}/contracts/ ${CMAKE_SOURCE_DIR}/tests/" CACHE PATH "Paths to the doxygen input")
  set(DOXY_HAVE_DOT "NO" CACHE STRING "Doxygen to use dot for diagrams.")
  if(DOXYGEN_DOT_FOUND)
    set(DOXY_HAVE_DOT "YES")
  endif()

  configure_file(doxyfile.in "${CMAKE_BINARY_DIR}/doxyfile")

  if(BUILD_DOXYGEN)
    message(STATUS "Doxygen found. Contracts documentation will be generated.")
    # Doxygen has issues making destination directories more than one level deep, so do it for it.
    add_custom_target(make_doc_dir ALL COMMAND ${CMAKE_COMMAND} -E make_directory "${DOXY_DOC_DEST_DIR}")
    add_custom_target(docs ALL
      COMMAND "${DOXYGEN_EXECUTABLE}" "${CMAKE_BINARY_DIR}/doxyfile"
      DEPENDS make_doc_dir
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      COMMENT "Building doxygen documentation in ${DOXY_DOC_DEST_DIR} ..."
      VERBATIM
    )
  endif(BUILD_DOXYGEN)
endif()
