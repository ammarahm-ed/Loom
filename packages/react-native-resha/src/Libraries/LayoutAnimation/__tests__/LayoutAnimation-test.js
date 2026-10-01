/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the LICENSE file.
 * @flow strict-local
 * @format
 */
'use strict';

jest.mock('../../../src/private/featureflags/ReactNativeFeatureFlags', () => ({
  isLayoutAnimationEnabled: () => true,
}));
jest.mock('../../ReactNative/UIManager', () => ({default: {}}));
jest.mock('../../ReactNative/FabricUIManager', () => ({
  getFabricUIManager: () => global.nativeFabricUIManager,
}));

const LayoutAnimation = require('../LayoutAnimation').default;

describe('Fabric LayoutAnimation callbacks', () => {
  beforeEach(() => jest.useFakeTimers());
  afterEach(() => {
    jest.useRealTimers();
    delete global.nativeFabricUIManager;
  });

  it('does not report success after native rejection', () => {
    global.nativeFabricUIManager = {
      unstable_forceNextCommitSync: jest.fn(),
      configureNextLayoutAnimation: (config, success, failure) => failure(),
    };
    const success = jest.fn();
    const failure = jest.fn();
    LayoutAnimation.configureNext({duration: 100}, success, failure);
    jest.runAllTimers();
    expect(failure).toHaveBeenCalledTimes(1);
    expect(success).not.toHaveBeenCalled();
  });

  it('settles native success and late failure exactly once', () => {
    global.nativeFabricUIManager = {
      unstable_forceNextCommitSync: jest.fn(),
      configureNextLayoutAnimation: (config, success, failure) => {
        success();
        failure();
      },
    };
    const success = jest.fn();
    const failure = jest.fn();
    LayoutAnimation.configureNext({duration: 100}, success, failure);
    jest.runAllTimers();
    expect(success).toHaveBeenCalledTimes(1);
    expect(failure).not.toHaveBeenCalled();
  });

  it('preserves the existing fallback outside the async direct renderer', () => {
    global.nativeFabricUIManager = {
      configureNextLayoutAnimation: (config, success, failure) => failure(),
    };
    const success = jest.fn();
    const failure = jest.fn();
    LayoutAnimation.configureNext({duration: 100}, success, failure);
    jest.runAllTimers();
    expect(failure).toHaveBeenCalledTimes(1);
    expect(success).toHaveBeenCalledTimes(1);
  });
});
