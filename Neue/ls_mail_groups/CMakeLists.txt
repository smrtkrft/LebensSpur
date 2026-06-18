idf_component_register(
    SRCS "src/ls_mail_groups.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES sk_core nvs_flash ls_smtp
)
