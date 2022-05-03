/*
 * Copyright (C) 2016 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <fwupdplugin.h>

#include "fu-steelseries-device.h"
#include "fu-steelseries-gamepad.h"

static void
fu_plugin_steelseries_init(FuPlugin *plugin)
{
	fu_plugin_add_device_gtype(plugin, FU_TYPE_STEELSERIES_DEVICE);
	fu_plugin_add_device_gtype(plugin, FU_TYPE_STEELSERIES_GAMEPAD);
}

static void
fu_plugin_steelseries_class_init(FuContext *ctx)
{
	fu_context_add_quirk_key(ctx, "SteelSeriesDeviceKind");
}

void
fu_plugin_init_vfuncs(FuPluginVfuncs *vfuncs)
{
	vfuncs->build_hash = FU_BUILD_HASH;
	vfuncs->class_init = fu_plugin_steelseries_class_init;
	vfuncs->init = fu_plugin_steelseries_init;
}
