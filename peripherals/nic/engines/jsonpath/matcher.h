/*
 * Copyright (C) 2013-2014 Jo-Philipp Wich
 * SPDX-License-Identifier: ISC
 */
#pragma once

#include "ast.h"
#include "../engine_compat.h"

typedef void (*jp_match_cb_t)(JSON_Value *res, void *priv);

JSON_Value *jp_match(struct jp_opcode *path, JSON_Value *jsobj, jp_match_cb_t cb, void *priv);
