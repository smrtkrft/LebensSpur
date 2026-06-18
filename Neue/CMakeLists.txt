cmake_minimum_required(VERSION 3.16)

# LebensSpur firmware. sk_core all-in-one SmartKraft baseline
# (USB CLI + BLE + WiFi/mDNS + TCP + secure session + button/LED I/O + OTA).
# sk_api: outbound HTTP (Telegram, IFTTT, generic webhook).
# LS-özgü component'lar (ls_timer_engine, ls_relay, ls_smtp, ls_mail_groups,
# ls_reset_api) Faz 1.1+ alt fazlarında listeye eklenecek.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/sk_core"
    "${CMAKE_CURRENT_LIST_DIR}/sk_api"
    "${CMAKE_CURRENT_LIST_DIR}/ls_timer_engine"
    "${CMAKE_CURRENT_LIST_DIR}/ls_relay"
    "${CMAKE_CURRENT_LIST_DIR}/ls_smtp"
    "${CMAKE_CURRENT_LIST_DIR}/ls_mail_groups"
    "${CMAKE_CURRENT_LIST_DIR}/ls_reminder"
    "${CMAKE_CURRENT_LIST_DIR}/ls_reset_api"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lebensspur)
