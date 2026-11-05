/* api — HTTP uç işleyicileri: JSON ↔ cfg / durum / komut (04-plan-son.md §6.2). */
#pragma once
#include <stddef.h>
#include "http.h"

const http_route_t *api_routes(size_t *count);
cJSON *api_status_json(void);          /* GET /api/status ve SSE aynı yükü kullanır */
