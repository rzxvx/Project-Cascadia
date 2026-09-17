# Find symbols the config fragment both enables and disables.
#
# merge_config takes the LAST setting of a symbol in a file, so a fragment that
# says CONFIG_X=y near the top and "# CONFIG_X is not set" near the bottom
# quietly ships X disabled.  That is not hypothetical: the fragment grew an
# "enable what this board needs" section and, separately, a "trim what
# multi_v7_defconfig drags in" section, and on 2026-09-17 the trim section was
# still turning off the NFS client that the section above it had just turned on
# for the NFS root.  The build's own symbol check caught it, but only after a
# full kernel compile.  This catches it in a second, and names both lines.
#
#   awk -f scripts/check-config-fragment.awk config/p105ap.config
#
# Exits 1 if anything contradicts itself.  A symbol repeated with the SAME
# value is fine and says nothing.

/^CONFIG_[A-Za-z0-9_]+=/ {
    s = $0; sub(/=.*/, "", s); sub(/^CONFIG_/, "", s); v = "enabled"
}
/^# CONFIG_[A-Za-z0-9_]+ is not set/ {
    s = $2; sub(/^CONFIG_/, "", s); v = "disabled"
}
s != "" {
    if (s in val && val[s] != v) {
        printf("    CONFLICT: CONFIG_%s is %s on line %d and %s on line %d\n",
               s, val[s], ln[s], v, NR)
        bad++
    }
    val[s] = v; ln[s] = NR; s = ""
}
END { exit (bad > 0 ? 1 : 0) }
