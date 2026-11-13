/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <react/renderer/uimanager/LoomPlatformTraits.h>

namespace facebook::react {

/**
 * The component vocabulary of hosts whose views are created by name through
 * ViewManager-style registries (the iOS and Android hosts): Fabric name to
 * view-manager name, mirroring `FabricNameComponentMapping.kt`. Only the
 * Fabric-to-manager direction exists here; it is not the React-view-name to
 * Fabric-name map.
 *
 * Entries for components this repository does not ship (ShimmeringView,
 * TemplateView, Video, Map, Keyframes, ImpressionTrackingView) are upstream's
 * own: an app whose view manager registers one of those names resolves exactly
 * as it does under Fabric. `Text` is deliberately not mapped: it is nested text,
 * a span inside its paragraph, and must not become a second text view.
 */
inline LoomComponentTable loomViewManagerComponentTable() {
  using Role = LoomComponentTable::Role;
  using Kind = LoomTextStateKind;
  auto table = LoomComponentTable::fabric();
  table.rawTextHostName = "RCTRawText";

  const auto add = [&](const char* name, uint16_t roles, Kind kind = Kind::Other) {
    table.entries[name] = LoomComponentTable::Entry{roles, kind};
  };
  add("RCTView", Role::Box);
  add("RCTText", Role::Paragraph, Kind::Text);
  add("RCTSelectableText", Role::Paragraph, Kind::SelectableParagraph);
  add("RCTVirtualText", Role::VirtualText);
  add("RCTRawText", Role::RawText);
  add("RCTScrollView", Role::GenericStateWrapper);
  add("RCTHorizontalScrollView", Role::GenericStateWrapper);
  add("AndroidHorizontalScrollView", Role::GenericStateWrapper);
  add("AndroidNestedScrollView", Role::GenericStateWrapper);
  add("RCTSafeAreaView", Role::GenericStateWrapper);
  add("RCTModalHostView", Role::GenericStateWrapper);
  add("AndroidProgressBar", Role::IntrinsicMeasurement);
  add("RCTSwitch", Role::IntrinsicMeasurement);
  add("AndroidSwitch", Role::IntrinsicMeasurement);
  add("RCTSlider", Role::IntrinsicMeasurement);
  add("AndroidHorizontalScrollContentView", Role::CoreRendered);
  add("AndroidTextInput", Role::CoreRendered);

  auto& host = table.hostNames;
  host["View"] = "RCTView";
  host["Image"] = "RCTImageView";
  host["ScrollView"] = "RCTScrollView";
  host["Paragraph"] = "RCTText";
  host["SelectableParagraph"] = "RCTSelectableText";
  // A Yoga-only context node with no view manager: the box needs an ordinary one.
  host["LayoutConformance"] = "RCTView";
  host["RawText"] = "RCTRawText";
  host["AndroidHorizontalScrollView"] = "RCTScrollView";
  host["ActivityIndicatorView"] = "AndroidProgressBar";
  host["ShimmeringView"] = "RKShimmeringView";
  host["TemplateView"] = "RCTTemplateView";
  host["AxialGradientView"] = "RCTAxialGradientView";
  host["ModalHostView"] = "RCTModalHostView";
  host["Slider"] = "RCTSlider";
  host["Video"] = "RCTVideo";
  host["Map"] = "RCTMap";
  host["WebView"] = "RCTWebView";
  host["Keyframes"] = "RCTKeyframes";
  host["ImpressionTrackingView"] = "RCTImpressionTrackingView";

  table.tagNames["AndroidTextInput"] = "TextInput";
  table.tagNames["AndroidSwitch"] = "Switch";
  return table;
}

} // namespace facebook::react
