/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @flow strict-local
 * @format
 */

import type {HostInstance} from '../../src/private/types/HostInstance';
import type {TextStyleProp} from '../StyleSheet/StyleSheet';
import type {TextStyle} from '../StyleSheet/StyleSheetTypes';
import type {GestureResponderEvent} from '../Types/CoreEventTypes';
import type {NativeTextProps} from './TextNativeComponent';
import type {PressRetentionOffset, TextProps} from './TextProps';

import * as ReactNativeFeatureFlags from '../../src/private/featureflags/ReactNativeFeatureFlags';
import * as PressabilityDebug from '../Pressability/PressabilityDebug';
import usePressability from '../Pressability/usePressability';
import flattenStyle from '../StyleSheet/flattenStyle';
import processColor from '../StyleSheet/processColor';
import StyleSheet from '../StyleSheet/StyleSheet';
import Platform from '../Utilities/Platform';
import TextAncestorContext from './TextAncestorContext';
import {
  NativeSelectableText,
  NativeText,
  NativeVirtualText,
} from './TextNativeComponent';
import * as React from 'react';
import {useContext, useMemo, useState} from 'react';
import {jsx} from 'react/jsx-runtime';

export type TextInstance = HostInstance;

export type {TextProps} from './TextProps';

/**
 * A React component for displaying text. `Text` supports nesting, styling,
 * and touch handling.
 *
 * `Text` uses text layout instead of flexbox — elements inside wrap at end of
 * line rather than being positioned as rectangles.
 *
 * @see https://reactnative.dev/docs/text
 */
// Loom: the props TextImpl handles itself. Everything else is copied through
// to the native text props. This replaces a 36-name rest destructure, which
// Hermes compiles into a freshly allocated exclusion object (one property per
// name) on every render, with one static lookup per prop.
const TEXT_IMPL_OWN_PROPS: {+[string]: true} = {
  'ref': true,
  'accessible': true,
  'accessibilityLabel': true,
  'accessibilityRole': true,
  'accessibilityState': true,
  'allowFontScaling': true,
  'aria-busy': true,
  'aria-checked': true,
  'aria-disabled': true,
  'aria-expanded': true,
  'aria-hidden': true,
  'aria-label': true,
  'aria-selected': true,
  'children': true,
  'ellipsizeMode': true,
  'disabled': true,
  'id': true,
  'nativeID': true,
  'numberOfLines': true,
  'onLongPress': true,
  'onPress': true,
  'onPressIn': true,
  'onPressOut': true,
  'onResponderGrant': true,
  'onResponderMove': true,
  'onResponderRelease': true,
  'onResponderTerminate': true,
  'onResponderTerminationRequest': true,
  'onStartShouldSetResponder': true,
  'pressRetentionOffset': true,
  'role': true,
  'selectable': true,
  'selectionColor': true,
  'suppressHighlighting': true,
  'style': true,
};

const TextImpl: component(
  ref?: React.RefSetter<TextInstance>,
  ...props: TextProps
) = (props: {
  ref?: React.RefSetter<TextInstance>,
  ...TextProps,
}) => {
  const restProps: {[string]: unknown} = {};
  for (const key in props) {
    if (TEXT_IMPL_OWN_PROPS[key] !== true) {
      restProps[key] = props[key];
    }
  }
  const forwardedRef = props.ref;
  const accessible = props.accessible;
  const accessibilityLabel = props.accessibilityLabel;
  const accessibilityRole = props.accessibilityRole;
  const accessibilityState = props.accessibilityState;
  const allowFontScaling = props.allowFontScaling;
  const ariaBusy = props['aria-busy'];
  const ariaChecked = props['aria-checked'];
  const ariaDisabled = props['aria-disabled'];
  const ariaExpanded = props['aria-expanded'];
  const ariaHidden = props['aria-hidden'];
  const ariaLabel = props['aria-label'];
  const ariaSelected = props['aria-selected'];
  const children = props.children;
  const ellipsizeMode = props.ellipsizeMode;
  const disabled = props.disabled;
  const id = props.id;
  const nativeID = props.nativeID;
  const numberOfLines = props.numberOfLines;
  const onLongPress = props.onLongPress;
  const onPress = props.onPress;
  const onPressIn = props.onPressIn;
  const onPressOut = props.onPressOut;
  const onResponderGrant = props.onResponderGrant;
  const onResponderMove = props.onResponderMove;
  const onResponderRelease = props.onResponderRelease;
  const onResponderTerminate = props.onResponderTerminate;
  const onResponderTerminationRequest = props.onResponderTerminationRequest;
  const onStartShouldSetResponder = props.onStartShouldSetResponder;
  const pressRetentionOffset = props.pressRetentionOffset;
  const role = props.role;
  const selectable = props.selectable;
  const selectionColor = props.selectionColor;
  const suppressHighlighting = props.suppressHighlighting;
  const style = props.style;
  const processedProps = restProps as {
    ...NativeTextProps,
  };
  const _accessibilityLabel = ariaLabel ?? accessibilityLabel;
  const _accessibilityStateDisabled =
    ariaDisabled ?? accessibilityState?.disabled;
  const _disabled = disabled ?? _accessibilityStateDisabled;

  // If the disabled prop and accessibilityState.disabled are out of sync but not both in
  // falsy states we need to update the accessibilityState object to use the disabled prop.
  const needsAccessibilityStateDisabledUpdate =
    _disabled !== _accessibilityStateDisabled &&
    ((_disabled != null && _disabled !== false) ||
      (_accessibilityStateDisabled != null &&
        _accessibilityStateDisabled !== false));

  let _accessibilityState: ?TextProps['accessibilityState'] =
    accessibilityState;
  if (
    needsAccessibilityStateDisabledUpdate ||
    ariaBusy != null ||
    ariaChecked != null ||
    ariaDisabled != null ||
    ariaExpanded != null ||
    ariaSelected != null
  ) {
    if (_accessibilityState != null) {
      _accessibilityState = {
        busy: ariaBusy ?? _accessibilityState.busy,
        checked: ariaChecked ?? _accessibilityState.checked,
        disabled: _disabled,
        expanded: ariaExpanded ?? _accessibilityState.expanded,
        selected: ariaSelected ?? _accessibilityState.selected,
      };
    } else {
      _accessibilityState = {
        busy: ariaBusy,
        checked: ariaChecked,
        disabled: _disabled,
        expanded: ariaExpanded,
        selected: ariaSelected,
      };
    }
  }

  if (ariaHidden !== undefined) {
    processedProps.accessibilityElementsHidden = ariaHidden;
    if (ariaHidden === true) {
      processedProps.importantForAccessibility = 'no-hide-descendants';
    }
  }

  // Loom: same result as `Platform.select({ios, android, default})`, without
  // allocating the options object on every render.
  const _accessible =
    Platform.OS === 'ios'
      ? accessible !== false
      : Platform.OS === 'android'
        ? accessible == null
          ? onPress != null || onLongPress != null
          : accessible
        : accessible;

  const isPressable =
    (onPress != null ||
      onLongPress != null ||
      onStartShouldSetResponder != null) &&
    _disabled !== true;

  const shouldUseLinkRole =
    isPressable && accessibilityRole == null && role == null;

  const _accessibilityRole =
    accessibilityRole ?? (shouldUseLinkRole ? 'link' : undefined);

  const _role = shouldUseLinkRole ? undefined : role;

  // TODO: Move this processing to the view configuration.
  const _selectionColor =
    selectionColor != null ? processColor(selectionColor) : undefined;

  let _style = style;
  if (__DEV__) {
    if (PressabilityDebug.isEnabled() && onPress != null) {
      _style = [style, {color: 'magenta'}];
    }
  }

  let _numberOfLines = numberOfLines;
  if (_numberOfLines != null && !(_numberOfLines >= 0)) {
    if (__DEV__) {
      console.error(
        `'numberOfLines' in <Text> must be a non-negative number, received: ${_numberOfLines}. The value will be set to 0.`,
      );
    }
    _numberOfLines = 0;
  }

  let _selectable = selectable;

  let processedStyle = flattenStyle<TextStyleProp>(_style);
  if (processedStyle != null) {
    let overrides: ?{...TextStyle} = null;
    if (typeof processedStyle.fontWeight === 'number') {
      overrides = overrides || ({} as {...TextStyle});
      overrides.fontWeight =
        // $FlowFixMe[incompatible-type]
        String(processedStyle.fontWeight) as TextStyle['fontWeight'];
    }

    if (processedStyle.userSelect != null) {
      _selectable = userSelectToSelectableMap[processedStyle.userSelect];
      overrides = overrides || ({} as {...TextStyle});
      overrides.userSelect = undefined;
    }

    if (processedStyle.verticalAlign != null) {
      overrides = overrides || ({} as {...TextStyle});
      overrides.textAlignVertical =
        verticalAlignToTextAlignVerticalMap[processedStyle.verticalAlign];
      overrides.verticalAlign = undefined;
    }

    if (overrides != null) {
      // $FlowFixMe[incompatible-type]
      _style = [_style, overrides];
    }
  }

  if (ReactNativeFeatureFlags.defaultTextToOverflowHidden()) {
    _style = [styles.default, _style];
  }

  const _nativeID = id ?? nativeID;

  if (_accessibilityLabel !== undefined) {
    processedProps.accessibilityLabel = _accessibilityLabel;
  }
  if (_accessibilityRole !== undefined) {
    processedProps.accessibilityRole = _accessibilityRole;
  }
  if (_accessibilityState !== undefined) {
    processedProps.accessibilityState = _accessibilityState;
  }
  if (_nativeID !== undefined) {
    processedProps.nativeID = _nativeID;
  }
  if (_numberOfLines !== undefined) {
    processedProps.numberOfLines = _numberOfLines;
  }
  if (_selectable !== undefined) {
    processedProps.selectable = _selectable;
  }
  if (_style !== undefined) {
    processedProps.style = _style;
  }
  if (_selectionColor !== undefined) {
    processedProps.selectionColor = _selectionColor;
  }
  if (_role !== undefined) {
    processedProps.role = _role;
  }

  let textPressabilityProps: ?TextPressabilityProps;
  if (isPressable) {
    textPressabilityProps = {
      onLongPress,
      onPress,
      onPressIn,
      onPressOut,
      onResponderGrant,
      onResponderMove,
      onResponderRelease,
      onResponderTerminate,
      onResponderTerminationRequest,
      onStartShouldSetResponder,
      pressRetentionOffset,
      suppressHighlighting,
    };
  }

  const hasTextAncestor = useContext(TextAncestorContext);
  if (hasTextAncestor) {
    processedProps.disabled = disabled;
    processedProps.children = children;
    if (isPressable) {
      return (
        <PressableVirtualText
          ref={forwardedRef}
          textProps={processedProps}
          textPressabilityProps={textPressabilityProps ?? {}}
        />
      );
    }
    // Loom: `processedProps` is this render's own rest copy; hand it to `jsx`
    // as the props object instead of copying it again with a JSX spread.
    processedProps.ref = forwardedRef;
    return jsx(NativeVirtualText, processedProps);
  }

  let nativeText = null;

  processedProps.accessible = _accessible;
  processedProps.allowFontScaling = allowFontScaling !== false;
  processedProps.disabled = _disabled;
  processedProps.ellipsizeMode = ellipsizeMode ?? 'tail';
  processedProps.children = children;

  if (isPressable) {
    nativeText = (
      <PressableText
        ref={forwardedRef}
        selectable={_selectable}
        textProps={processedProps}
        textPressabilityProps={textPressabilityProps ?? {}}
      />
    );
  } else {
    // Loom: see NativeVirtualText above; same props, one copy fewer.
    processedProps.ref = forwardedRef;
    nativeText = jsx(
      _selectable === true ? NativeSelectableText : NativeText,
      processedProps,
    );
  }

  if (children == null) {
    return nativeText;
  }

  // If the children do not contain a JSX element it would not be possible to have a
  // nested `Text` component so we can skip adding the `TextAncestorContext` context wrapper
  // which has a performance overhead. Since we do this for performance reasons we need
  // to keep the check simple to avoid regressing overall perf. For this reason the
  // `children.length` constant is set to `3`, this should be a reasonable tradeoff
  // to capture the majority of `Text` uses but also not make this check too expensive.
  if (Array.isArray(children) && children.length <= 3) {
    let hasNonTextChild = false;
    for (let child of children) {
      if (child != null && typeof child === 'object') {
        hasNonTextChild = true;
        break;
      }
    }
    if (!hasNonTextChild) {
      return nativeText;
    }
  } else if (typeof children !== 'object') {
    return nativeText;
  }

  return <TextAncestorContext value={true}>{nativeText}</TextAncestorContext>;
};

TextImpl.displayName = 'Text';

type TextPressabilityProps = Readonly<{
  onLongPress?: ?(event: GestureResponderEvent) => unknown,
  onPress?: ?(event: GestureResponderEvent) => unknown,
  onPressIn?: ?(event: GestureResponderEvent) => unknown,
  onPressOut?: ?(event: GestureResponderEvent) => unknown,
  onResponderGrant?: ?(event: GestureResponderEvent) => void,
  onResponderMove?: ?(event: GestureResponderEvent) => void,
  onResponderRelease?: ?(event: GestureResponderEvent) => void,
  onResponderTerminate?: ?(event: GestureResponderEvent) => void,
  onResponderTerminationRequest?: ?() => boolean,
  onStartShouldSetResponder?: ?() => boolean,
  pressRetentionOffset?: ?PressRetentionOffset,
  suppressHighlighting?: ?boolean,
}>;

/**
 * Hook that handles setting up Pressability of Text components.
 *
 * NOTE: This hook is relatively expensive so it should only be used absolutely necessary.
 */
function useTextPressability({
  onLongPress,
  onPress,
  onPressIn,
  onPressOut,
  onResponderGrant,
  onResponderMove,
  onResponderRelease,
  onResponderTerminate,
  onResponderTerminationRequest,
  onStartShouldSetResponder,
  pressRetentionOffset,
  suppressHighlighting,
}: TextPressabilityProps) {
  const [isHighlighted, setHighlighted] = useState(false);

  // Setup pressability config and wrap callbacks needs to track the highlight state.
  const config = useMemo(() => {
    let _onPressIn = onPressIn;
    let _onPressOut = onPressOut;

    // Updating isHighlighted causes unnecessary re-renders for platforms that don't use it
    // in the best case, and cause issues with text selection in the worst case. Forcing
    // the isHighlighted prop to false on all platforms except iOS.
    if (Platform.OS === 'ios') {
      _onPressIn = (event: GestureResponderEvent) => {
        setHighlighted(suppressHighlighting == null || !suppressHighlighting);
        onPressIn?.(event);
      };

      _onPressOut = (event: GestureResponderEvent) => {
        setHighlighted(false);
        onPressOut?.(event);
      };
    }

    return {
      disabled: false,
      pressRectOffset: pressRetentionOffset,
      onLongPress,
      onPress,
      onPressIn: _onPressIn,
      onPressOut: _onPressOut,
    };
  }, [
    pressRetentionOffset,
    onLongPress,
    onPress,
    onPressIn,
    onPressOut,
    suppressHighlighting,
  ]);

  // Init the pressability class
  const eventHandlers = usePressability(config);

  // Create NativeText event handlers which proxy events to pressability
  const eventHandlersForText = useMemo(
    () =>
      eventHandlers == null
        ? null
        : {
            onResponderGrant(event: GestureResponderEvent) {
              eventHandlers.onResponderGrant(event);
              if (onResponderGrant != null) {
                onResponderGrant(event);
              }
            },
            onResponderMove(event: GestureResponderEvent) {
              eventHandlers.onResponderMove(event);
              if (onResponderMove != null) {
                onResponderMove(event);
              }
            },
            onResponderRelease(event: GestureResponderEvent) {
              eventHandlers.onResponderRelease(event);
              if (onResponderRelease != null) {
                onResponderRelease(event);
              }
            },
            onResponderTerminate(event: GestureResponderEvent) {
              eventHandlers.onResponderTerminate(event);
              if (onResponderTerminate != null) {
                onResponderTerminate(event);
              }
            },
            onClick: eventHandlers.onClick,
            onResponderTerminationRequest:
              onResponderTerminationRequest != null
                ? onResponderTerminationRequest
                : eventHandlers.onResponderTerminationRequest,
            onStartShouldSetResponder:
              onStartShouldSetResponder != null
                ? onStartShouldSetResponder
                : eventHandlers.onStartShouldSetResponder,
          },
    [
      eventHandlers,
      onResponderGrant,
      onResponderMove,
      onResponderRelease,
      onResponderTerminate,
      onResponderTerminationRequest,
      onStartShouldSetResponder,
    ],
  );

  // Return the highlight state and NativeText event handlers
  return useMemo(
    () => [isHighlighted, eventHandlersForText],
    [isHighlighted, eventHandlersForText],
  );
}

/**
 * Wrap the NativeVirtualText component and initialize pressability.
 *
 * This logic is split out from the main Text component to enable the more
 * expensive pressability logic to be only initialized when needed.
 */
component PressableVirtualText(
  ref?: React.RefSetter<TextInstance>,
  textProps: NativeTextProps,
  textPressabilityProps: TextPressabilityProps,
) {
  const [isHighlighted, eventHandlersForText] = useTextPressability(
    textPressabilityProps,
  );

  return (
    <NativeVirtualText
      {...textProps}
      {...eventHandlersForText}
      isHighlighted={isHighlighted}
      isPressable={true}
      ref={ref}
    />
  );
}

/**
 * Wrap a NativeText component and initialize pressability.
 *
 * This logic is split out from the main Text component to enable the more
 * expensive pressability logic to be only initialized when needed.
 */
component PressableText(
  ref?: React.RefSetter<TextInstance>,
  selectable?: ?boolean,
  textProps: NativeTextProps,
  textPressabilityProps: TextPressabilityProps,
) {
  const [isHighlighted, eventHandlersForText] = useTextPressability(
    textPressabilityProps,
  );

  const NativeComponent =
    selectable === true ? NativeSelectableText : NativeText;

  return (
    <NativeComponent
      {...textProps}
      {...eventHandlersForText}
      isHighlighted={isHighlighted}
      isPressable={true}
      ref={ref}
    />
  );
}

const userSelectToSelectableMap = {
  auto: true,
  text: true,
  none: false,
  contain: true,
  all: true,
};

const verticalAlignToTextAlignVerticalMap = {
  auto: 'auto',
  top: 'top',
  bottom: 'bottom',
  middle: 'center',
} as const;

const styles = StyleSheet.create({
  // Native components have historically acted like overflow: 'hidden'. We set
  // this, as part of the default style, to let client differentiate with
  // overflow: 'visible'.
  default: {
    overflow: 'hidden',
  },
});

export default TextImpl;
