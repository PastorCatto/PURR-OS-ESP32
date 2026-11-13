idf_component_register(SRCS "src/purr_util.c" "src/purr_decision.c" "src/purr_keybag.c"
                            "src/purr_cfg.c" "src/purr_verify.c" "src/purr_menu.c"
                            "src/purr_term.c" "src/purr_cli.c" "src/purr_wifi.c" "src/purr_manifest.c" "keys/purr_default_keys.c" "src/purr_pbkdf2.c" "src/purr_users.c" "src/purr_appmgr.c" "src/purr_relocate.c" "src/purr_module.c" "src/purr_shellhdr.c" "src/purr_swap.c" "src/purr_trailer.c" "src/purr_trust.c" "src/purr_runtime.c" "src/purr_repo.c" "src/purr_repo_trust.c" "src/purr_move.c" "src/purr_resetnote.c" "src/purr_power.c" "src/purr_perm.c" "src/purr_msgstore.c" "src/purr_contacts.c" "src/purr_reason.c" "src/purr_hwscan.c" "src/purr_hwprofile.c" "src/purr_bootimg.c" "src/purr_statusicons.c" "src/purr_battmodel.c" "src/purr_pin.c"
                       INCLUDE_DIRS "include")

# purr_appmgr.h needs purr_fs.h, which lives in the kernel component. CoreOS runs on top of
# the kernel (the kernel starts it), so depending on the kernel's filesystem API here is the
# right way around -- but a formal REQUIRES would be circular, since kernel already REQUIRES
# coreos. Reach into its include dir directly instead, the same way the bootloader reaches
# into coreos's headers without a formal component dependency.
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../kernel/include"
    "${CMAKE_CURRENT_LIST_DIR}/../littlefs")
