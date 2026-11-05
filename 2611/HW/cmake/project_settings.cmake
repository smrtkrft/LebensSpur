# Proje geneli derleme ayarlari (esp_bin/2611/CMakeLists.txt project() oncesi dahil eder).

# cJSON ic ice siniri: varsayilan 1000 seviye, 80 B/seviye -> 2 KB "[[[[" govdesi
# 8 KB httpd yiginini tasirir (03-arastirma/01 §5). Tum bilesenlere (json dahil) uygulanir.
idf_build_set_property(COMPILE_DEFINITIONS "CJSON_NESTING_LIMIT=16" APPEND)

# Proje bilesenleri icin siki uyari seti. IDF -Werror'u zaten kuresel acar ama bazi
# uyarilari gevsetir; proje kodunda hepsi hata sayilir.
function(ls_strict_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Werror -Wshadow -Wpointer-arith -Wcast-align
        -Werror=unused-variable -Werror=unused-function -Werror=unused-but-set-variable
        -Wno-error=deprecated-declarations)
endfunction()
