# Standalone test application for the wlink component (modern "test_apps"
# layout). Build/flash from *inside this directory* with idf.py, or let pytest
# do it. This project lives next to the component it tests, so nothing central
# has to know it exists.
cmake_minimum_required(VERSION 3.16)

# The component under test (and its siblings) live two levels up in components/.
# The project root is here, so IDF won't auto-discover them without this.
set(EXTRA_COMPONENT_DIRS "../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wlink_sta_test)
