/**
 * Bench entry with the Fabric call timing probe (see benchFabricProbe.js). Not the default entry:
 * the probe slows every native UI manager call.
 *
 * @format
 */

import { installFabricProbe } from './benchFabricProbe';

installFabricProbe();
require('./index');
