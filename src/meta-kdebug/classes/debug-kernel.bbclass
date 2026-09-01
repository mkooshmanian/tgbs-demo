python __anonymous () {
    if not bb.utils.contains('DISTRO_FEATURES', 'kdebug', True, False, d):
        return

    pn = d.getVar('PN') or ''
    preferred_kernel = d.getVar('PREFERRED_PROVIDER_virtual/kernel') or ''
    provides = (d.getVar('PROVIDES') or '').split()

    if pn != preferred_kernel and 'virtual/kernel' not in provides:
        return

    files_dir = d.getVar('DEBUG_KERNEL_FILES_DIR')
    if files_dir:
        d.prependVar('FILESEXTRAPATHS', files_dir + ':')

    d.appendVar('SRC_URI', ' file://debug_info.cfg')

    # Non-kernel-yocto recipes can expose this variable to merge fragments
    # explicitly. linux-tgbs uses it for its custom configuration flow.
    if d.getVar('KERNEL_CONFIG_FRAGMENTS') is not None:
        d.appendVar('KERNEL_CONFIG_FRAGMENTS', ' ${WORKDIR}/debug_info.cfg')

    d.appendVarFlag('do_configure', 'postfuncs', ' debug_kernel_check_config')
    d.appendVarFlag('do_compile', 'postfuncs', ' debug_kernel_do_compile')
}

debug_kernel_check_config() {
    if [ ! -f "${B}/.config" ] || \
       ! grep -q '^CONFIG_GDB_SCRIPTS=y$' "${B}/.config"; then
        bbfatal "kdebug is enabled but CONFIG_GDB_SCRIPTS is missing from the final kernel configuration"
    fi
}

debug_kernel_do_compile() {
    if [ -f "${B}/.config" ] && grep -q '^CONFIG_GDB_SCRIPTS=y$' "${B}/.config"; then
        if [ -f "${S}/Makefile" ] && grep -q '^scripts_gdb:' "${S}/Makefile"; then
            oe_runmake ${PARALLEL_MAKE} -C "${S}" O="${B}" scripts_gdb
        fi

        if [ ! -f "${S}/scripts/gdb/vmlinux-gdb.py" ]; then
            return 0
        fi

        rm -f "${B}/vmlinux-gdb.py"
        install -m 0644 "${S}/scripts/gdb/vmlinux-gdb.py" "${B}/vmlinux-gdb.py"
        install -d "${B}/scripts/gdb/linux"
        for script in "${S}"/scripts/gdb/linux/*.py; do
            [ -e "$script" ] || continue
            install -m 0644 "$script" "${B}/scripts/gdb/linux/"
        done
    fi
}
