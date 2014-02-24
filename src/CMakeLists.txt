ADD_EXECUTABLE(emlin
emlin.c
dh.c
)

TARGET_LINK_LIBRARIES(emlin emelf)

install(TARGETS emlin
	RUNTIME DESTINATION bin
)

# vim: tabstop=4
