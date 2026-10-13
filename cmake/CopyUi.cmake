# Copies the built web UI next to the executable. Missing ui/dist is not an error (prints a hint).
if(EXISTS "${SRC}/index.html")
  file(REMOVE_RECURSE "${DST}")
  file(COPY "${SRC}/" DESTINATION "${DST}")
else()
  message(STATUS "ui/dist not found - run `npm run build` in ui/ to bundle the UI next to the executable")
endif()
