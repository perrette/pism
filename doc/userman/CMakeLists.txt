# User's Manual:

# Create the revision stamp:
file (WRITE ${CMAKE_CURRENT_BINARY_DIR}/revision.tex "${Pism_REVISION_TAG}")

# Copy the bibliography file (note the change in the file name)
configure_file (../ice_bib.bib ice-bib.bib COPYONLY)

# Find all the .tex files in userman and copy them to the build directory
file (GLOB parts
  RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
  *.tex)
foreach (filename ${parts})
  configure_file(${filename} ${filename} COPYONLY)
endforeach()

# Find all the figures and copy them to the build directory
foreach (format jpg png pdf)
  file (GLOB ${format}
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/figs
    figs/*.${format})
endforeach()
foreach (filename ${jpg} ${png} ${pdf})
  configure_file(figs/${filename} figs/${filename} COPYONLY)
endforeach()

# Commands to build the manual:
add_custom_command (OUTPUT manual.pdf
  COMMAND ${PDFLATEX_COMPILER} manual.tex
  COMMAND ${BIBTEX_COMPILER} manual
  COMMAND ${MAKEINDEX_COMPILER} -l manual
  COMMAND ${MAKEINDEX_COMPILER} -l -o manual.ond manual.odx
  COMMAND ${PDFLATEX_COMPILER} manual.tex
  COMMAND ${PDFLATEX_COMPILER} manual.tex
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_custom_target(manual ALL
  DEPENDS manual.pdf)