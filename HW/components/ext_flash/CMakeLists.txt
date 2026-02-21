set(GUI_DIR "${CMAKE_SOURCE_DIR}/GUI")

idf_component_register(
    SRCS "ext_flash.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver spi_flash littlefs vfs nvs_flash
    EMBED_TXTFILES
        "${GUI_DIR}/index.html"
        "${GUI_DIR}/style.css"
        "${GUI_DIR}/app.js"
        "${GUI_DIR}/config.json"
        "${GUI_DIR}/lang/en.json"
        "${GUI_DIR}/lang/de.json"
        "${GUI_DIR}/lang/es.json"
        "${GUI_DIR}/lang/fr.json"
        "${GUI_DIR}/lang/it.json"
        "${GUI_DIR}/lang/tr.json"
    EMBED_FILES
        "${GUI_DIR}/pic/logo.png"
        "${GUI_DIR}/pic/darklogo.png"
)
