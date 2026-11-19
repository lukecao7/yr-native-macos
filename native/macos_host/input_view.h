// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#import <AppKit/AppKit.h>

#include "session.h"

// Converts AppKit events to viewport pixels with a top-left origin.
@interface CoastalInputView : NSView
- (instancetype)initWithFrame:(NSRect)rect session:(coastal::macos_host::Session*)session;
@end
