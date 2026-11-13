import {StyleSheet} from 'react-native-unistyles';

// Two themes that differ in a layout value (width) and a paint value (color), so a theme switch
// is observable through measurement.
const light = {box: 100, color: '#cc3333'};
const dark = {box: 200, color: '#3333cc'};

type AppThemes = {light: typeof light; dark: typeof dark};
declare module 'react-native-unistyles' {
  export interface UnistylesThemes extends AppThemes {}
}

StyleSheet.configure({themes: {light, dark}, settings: {initialTheme: 'light'}});
