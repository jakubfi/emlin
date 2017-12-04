ADD_EXECUTABLE(emlin
	emlin.c
	dh.c
)

include_directories(${EMELF_INCLUDE_DIRS})
TARGET_LINK_LIBRARIES(emlin ${EMELF_LIBRARIES})

install(TARGETS emlin
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# vim: tabstop=4
