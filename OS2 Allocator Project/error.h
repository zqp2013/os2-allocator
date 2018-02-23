#pragma once

typedef struct error {
	bool occured = false;
	char function[64] = "none\0";
} error;