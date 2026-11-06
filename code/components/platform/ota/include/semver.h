/*
 * semver — sürüm karşılaştırma (saf C, host testli). En çok 4 sayısal parça (LebensSpur: 1.0.0, ara yayın 1.0.0.1),
 * "v" öneki, eksik parçalar (1.2 = 1.2.0.0),
 * ön sürüm (1.0.0-rc.1 < 1.0.0, SemVer §11) ve yapı eki (+abc yok sayılır) kabul edilir.
 */
#pragma once
#include <stdbool.h>

/* -1: a < b, 0: eşit, 1: a > b. Geçersiz sürüm her geçerliden küçük sayılır. */
int  semver_cmp(const char *a, const char *b);
bool semver_valid(const char *v);
