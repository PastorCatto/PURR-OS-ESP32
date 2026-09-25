# Rewritten onto the uiconf declarative UI-config format (see
# /home/PastorCatto/.claude/plans/delightful-exploring-hearth.md) — REQUIRES
# scrubbed down from the previous five-tabs-merged version's own union of
# driver_manager/bt_mgr/meshtastic/lib_mesh_pb, none of which this
# first-pass, read-only screen touches. Those come back (and this list
# grows again) as the deferred Hardware/Services/Mesh/Tasks tabs are
# rebuilt on the new format in a later pass.
idf_component_register(
    SRCS
        diagnostics.c
    INCLUDE_DIRS
        .
        ../../../kernel/core
        ../../../kernel/catcalls
        ../../../modules/app_manager
        ../../../modules/uiconf
    REQUIRES esp_common driver freertos app_manager uiconf
)
