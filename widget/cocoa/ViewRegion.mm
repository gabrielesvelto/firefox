/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ViewRegion.h"
#import <Cocoa/Cocoa.h>

#include "nsCocoaWindow.h"

using namespace mozilla;

ViewRegion::~ViewRegion() {
  for (NSView* view : mViews) {
    [view removeFromSuperview];
  }
}

bool ViewRegion::UpdateRegion(const LayoutDeviceIntRegion& aRegion,
                              const nsCocoaWindow& aCoordinateConverter,
                              NSView* aContainerView,
                              NSView* (^aViewCreationCallback)()) {
  if (mRegion == aRegion) {
    return false;
  }

  // We need to construct the required region using as many EffectViews
  // as necessary. We try to update the geometry of existing views if
  // possible, or create new ones or remove old ones if the number of
  // rects in the region has changed.

  nsTArray<NSView*> viewsToRecycle = std::move(mViews);
  // The mViews array is now empty.

  size_t viewsRecycled = 0;
  for (auto iter = aRegion.RectIter(); !iter.Done(); iter.Next()) {
    NSRect rect = aCoordinateConverter.DevPixelsToCocoaPoints(iter.Get());
    NSView* view = nil;
    if (viewsRecycled < viewsToRecycle.Length()) {
      view = viewsToRecycle[viewsRecycled++];
    } else {
      view = aViewCreationCallback();
      [aContainerView addSubview:view];

      // Now that the view is in the view hierarchy, it'll be kept alive by
      // its superview, so we can drop our reference.
      [view release];
    }
    if (!NSEqualRects(rect, view.frame)) {
      view.frame = rect;
    }
    view.needsDisplay = YES;
    mViews.AppendElement(view);
  }
  for (NSView* view : Span(viewsToRecycle).From(viewsRecycled)) {
    // Our new region is made of fewer rects than the old region, so we can
    // remove this view. We only have a weak reference to it, so removing it
    // from the view hierarchy will release it.
    [view removeFromSuperview];
  }

  mRegion = aRegion;
  return true;
}
