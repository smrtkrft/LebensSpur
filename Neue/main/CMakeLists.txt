idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES sk_core sk_api nvs_flash
                  ls_timer_engine ls_relay ls_smtp ls_mail_groups
                  ls_reminder ls_reset_api
)
