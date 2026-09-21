idf_component_register(
    SRCS "test_wlink_sta.c"
    INCLUDE_DIRS "."
    # unity provides TEST_CASE + the serial menu; wlink is the code under test.
    REQUIRES unity wlink nvs_flash
    # TEST_CASE registers itself via a linker section referenced by nothing at
    # link time. Without WHOLE_ARCHIVE the linker garbage-collects the cases and
    # the menu comes up empty. This is the bit people forget.
    WHOLE_ARCHIVE
)
