/* api iç: rota alt tabloları (api.c birleştirir) */
#pragma once
#include <stddef.h>
#include "http.h"
extern const http_route_t api_group_routes[];
extern const size_t api_group_routes_n;
extern const http_route_t api_sys_routes[];
extern const size_t api_sys_routes_n;
