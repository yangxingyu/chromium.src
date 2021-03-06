// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_COCOA_LOCATION_BAR_SEARCH_BUTTON_DECORATION_H_
#define CHROME_BROWSER_UI_COCOA_LOCATION_BAR_SEARCH_BUTTON_DECORATION_H_

#import <Cocoa/Cocoa.h>

#include "chrome/browser/ui/cocoa/location_bar/button_decoration.h"

class LocationBarViewMac;

// |SearchButtonDecoration| adds a search/go button to the right of the omnibox.

class SearchButtonDecoration : public ButtonDecoration {
 public:
  explicit SearchButtonDecoration(LocationBarViewMac* owner);
  ~SearchButtonDecoration() override;

  // Implement |LocationBarDecoration|.
  CGFloat GetWidthForSpace(CGFloat width) override;
  void DrawInFrame(NSRect frame, NSView* control_view) override;
  bool OnMousePressed(NSRect frame, NSPoint location) override;

 private:
  // The control view that owns this. Weak.
  LocationBarViewMac* owner_;

  DISALLOW_COPY_AND_ASSIGN(SearchButtonDecoration);
};

#endif  // CHROME_BROWSER_UI_COCOA_LOCATION_BAR_SEARCH_BUTTON_DECORATION_H_
