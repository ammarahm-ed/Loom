/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @flow strict-local
 * @format
 */

import type {HostInstance} from '../../../src/private/types/HostInstance';
import type {SafeAreaInsetsChangeEvent} from '../../Types/CoreEventTypes';
import type {ViewProps} from './ViewPropTypes';

import TextAncestorContext from '../../Text/TextAncestorContext';
import ViewNativeComponent from './ViewNativeComponent';
import * as React from 'react';
import {use} from 'react';
import {jsx} from 'react/jsx-runtime';

const warnOnRepeatedSafeAreaInsetsChanges: (
  onSafeAreaInsetsChange: (event: SafeAreaInsetsChangeEvent) => unknown,
) => (event: SafeAreaInsetsChangeEvent) => unknown = __DEV__
  ? require('../../../src/private/components/view/warnOnRepeatedSafeAreaInsetsChanges')
      .default
  : onSafeAreaInsetsChange => onSafeAreaInsetsChange;

export type ViewInstance = HostInstance;

/**
 * The most fundamental component for building a UI. `View` is a container that
 * supports layout with flexbox, style, some touch handling, and accessibility
 * controls. `View` maps directly to the native view equivalent on whatever
 * platform React Native is running on, whether that is a `UIView`, `<div>`,
 * `android.view`, etc. `View` is designed to be nested inside other views and
 * can have 0 to many children of any type.
 *
 * @see https://reactnative.dev/docs/view
 */
const VIEW_OWN_PROPS: {+[string]: true} = {
  'accessibilityState': true,
  'accessibilityValue': true,
  'aria-busy': true,
  'aria-checked': true,
  'aria-disabled': true,
  'aria-expanded': true,
  'aria-hidden': true,
  'aria-label': true,
  'aria-labelledby': true,
  'aria-live': true,
  'aria-modal': true,
  'aria-selected': true,
  'aria-valuemax': true,
  'aria-valuemin': true,
  'aria-valuenow': true,
  'aria-valuetext': true,
  'id': true,
  'tabIndex': true,
};

// Loom: written as a forwardRef render function rather than with component
// syntax. Component syntax compiles to `function View_withRef({...props}, ref)`,
// which copies every prop once more before the rest destructuring below copies
// them again; React already hands the render function a props object of its
// own, so that first copy is pure overhead on every View render.
function View_withRef(
  props: ViewProps,
  ref?: React.RefSetter<ViewInstance>,
): React.Node {
  const hasTextAncestor = use(TextAncestorContext);

  // Loom: most Views pass none of the props this component rewrites, and no
  // ref. Their props are then already exactly the native component's props, so
  // they are handed on as they are instead of being copied key by key. (React
  // treats props objects as immutable, so sharing one between this component
  // and the host component is safe.) Development keeps the copy: it wraps
  // `experimental_onSafeAreaInsetsChange` in place.
  if (!__DEV__ && ref == null) {
    let rewritesProps = false;
    for (const key in props) {
      if (VIEW_OWN_PROPS[key] === true) {
        rewritesProps = true;
        break;
      }
    }
    if (!rewritesProps) {
      // $FlowFixMe[incompatible-type] ViewProps without the rewritten keys
      const plainView = jsx(ViewNativeComponent, props);
      return hasTextAncestor ? (
        <TextAncestorContext value={false}>{plainView}</TextAncestorContext>
      ) : (
        plainView
      );
    }
  }

  // Loom: one static lookup per prop instead of an 18-name rest destructure,
  // which Hermes compiles into a fresh exclusion object on every render.
  const otherProps: {[string]: unknown} = {};
  for (const key in props) {
    if (VIEW_OWN_PROPS[key] !== true) {
      otherProps[key] = props[key];
    }
  }
  const accessibilityState = props.accessibilityState;
  const accessibilityValue = props.accessibilityValue;
  const ariaBusy = props['aria-busy'];
  const ariaChecked = props['aria-checked'];
  const ariaDisabled = props['aria-disabled'];
  const ariaExpanded = props['aria-expanded'];
  const ariaHidden = props['aria-hidden'];
  const ariaLabel = props['aria-label'];
  const ariaLabelledBy = props['aria-labelledby'];
  const ariaLive = props['aria-live'];
  const ariaModal = props['aria-modal'];
  const ariaSelected = props['aria-selected'];
  const ariaValueMax = props['aria-valuemax'];
  const ariaValueMin = props['aria-valuemin'];
  const ariaValueNow = props['aria-valuenow'];
  const ariaValueText = props['aria-valuetext'];
  const id = props.id;
  const tabIndex = props.tabIndex;

  const resolvedProps = otherProps as {...ViewProps};

  const parsedAriaLabelledBy = ariaLabelledBy?.split(/\s*,\s*/g);
  if (parsedAriaLabelledBy !== undefined) {
    resolvedProps.accessibilityLabelledBy = parsedAriaLabelledBy;
  }

  if (ariaLabel !== undefined) {
    resolvedProps.accessibilityLabel = ariaLabel;
  }

  if (ariaLive !== undefined) {
    resolvedProps.accessibilityLiveRegion =
      ariaLive === 'off' ? 'none' : ariaLive;
  }

  if (ariaModal !== undefined) {
    resolvedProps.accessibilityViewIsModal = ariaModal;
  }

  if (ariaHidden !== undefined) {
    resolvedProps.accessibilityElementsHidden = ariaHidden;
    if (ariaHidden === true) {
      resolvedProps.importantForAccessibility = 'no-hide-descendants';
    }
  }

  if (id !== undefined) {
    resolvedProps.nativeID = id;
  }

  if (tabIndex !== undefined) {
    resolvedProps.focusable = !tabIndex;
  }

  if (
    accessibilityState != null ||
    ariaBusy != null ||
    ariaChecked != null ||
    ariaDisabled != null ||
    ariaExpanded != null ||
    ariaSelected != null
  ) {
    resolvedProps.accessibilityState = {
      busy: ariaBusy ?? accessibilityState?.busy,
      checked: ariaChecked ?? accessibilityState?.checked,
      disabled: ariaDisabled ?? accessibilityState?.disabled,
      expanded: ariaExpanded ?? accessibilityState?.expanded,
      selected: ariaSelected ?? accessibilityState?.selected,
    };
  }

  if (
    accessibilityValue != null ||
    ariaValueMax != null ||
    ariaValueMin != null ||
    ariaValueNow != null ||
    ariaValueText != null
  ) {
    resolvedProps.accessibilityValue = {
      max: ariaValueMax ?? accessibilityValue?.max,
      min: ariaValueMin ?? accessibilityValue?.min,
      now: ariaValueNow ?? accessibilityValue?.now,
      text: ariaValueText ?? accessibilityValue?.text,
    };
  }

  if (__DEV__) {
    const onSafeAreaInsetsChange =
      resolvedProps.experimental_onSafeAreaInsetsChange;
    if (onSafeAreaInsetsChange != null) {
      resolvedProps.experimental_onSafeAreaInsetsChange =
        warnOnRepeatedSafeAreaInsetsChanges(onSafeAreaInsetsChange);
    }
  }

  // Loom: `resolvedProps` is this render's own object (the rest copy above),
  // so it becomes the element's props as is. JSX spread would copy it again;
  // `jsx` keeps a config without `key` as the props object. Same props, same
  // `ref` presence as `<ViewNativeComponent {...resolvedProps} ref={ref} />`.
  if (ref != null) {
    // $FlowFixMe[prop-missing] ref travels as a prop (React 19)
    resolvedProps.ref = ref;
  }
  const actualView = jsx(ViewNativeComponent, resolvedProps);

  if (hasTextAncestor) {
    return (
      <TextAncestorContext value={false}>{actualView}</TextAncestorContext>
    );
  }
  return actualView;
}

const View: component(
  ref?: React.RefSetter<ViewInstance>,
  ...props: ViewProps
) = React.forwardRef(View_withRef) as $FlowFixMe;

View.displayName = 'View';

export default View;
