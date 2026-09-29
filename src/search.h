#pragma once

#include <Arduino.h>
#include "squish_db.h"

// Live prefix results shown in the tap list / Lua search API.
static const size_t SEARCH_MAX_RESULTS = 12;

// Preferred path: app package on LittleFS (data/apps/squish_id/catalog.csv → uploadfs).
static const char* SQUISH_APP_CSV_PATH = "/apps/squish_id/catalog.csv";
// Fallback basename at LittleFS root (legacy / optional).
static const char* SQUISH_LITTLEFS_CSV_PATH = "/squishmallows.csv";

struct SquishRecord {
  char name[40];
  char full_name[72];
  char animal[48];
  char squad[40];
  char size[12];
  char retail_price_usd[12];
  char bio[160];
};

enum class CatalogSource : uint8_t { None, LittleFs, Fallback };

bool catalogInit();
CatalogSource catalogSource();
size_t catalogCount();

// Case-insensitive prefix match on name or full_name. Fills out[] up to maxOut.
size_t searchPrefix(const char* query, SquishRecord* out, size_t maxOut);

void squishCopyStr(char* dest, size_t destSize, const char* src);
