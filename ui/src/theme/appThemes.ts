/**
 * Built-in application (chrome/UI) themes. Every AppThemeColors key maps to a CSS variable.
 * (Expanded by the shell implementation: dark, light, plus additional curated themes.)
 */
import type { AppTheme } from '@shared/types';

export const APP_THEMES: AppTheme[] = [
  {
    id: 'dark',
    name: 'Dark',
    variant: 'dark',
    defaultScheme: 'one-dark',
    colors: {
      bg: '#1b1d22',
      bgElevated: '#23262d',
      bgSunken: '#15171b',
      surface: '#2a2e36',
      surfaceHover: '#323742',
      surfaceActive: '#3a404c',
      border: '#31353e',
      borderStrong: '#454b57',
      text: '#e6e8ec',
      textMuted: '#a3a9b5',
      textFaint: '#6f7684',
      accent: '#4c8dff',
      accentHover: '#6aa1ff',
      accentFg: '#ffffff',
      danger: '#f2555a',
      warning: '#e3a23b',
      success: '#3fb968',
      info: '#4c8dff',
      titlebar: '#15171b',
      titlebarFg: '#c9ced8',
      sidebar: '#191b20',
      tabActive: '#1b1d22',
      tabHover: '#22252b',
      overlay: 'rgba(0,0,0,0.5)',
      scrollbar: 'rgba(255,255,255,0.14)',
      scrollbarHover: 'rgba(255,255,255,0.26)'
    }
  },
  {
    id: 'light',
    name: 'Light',
    variant: 'light',
    defaultScheme: 'one-light',
    colors: {
      bg: '#ffffff',
      bgElevated: '#ffffff',
      bgSunken: '#f3f4f6',
      surface: '#f3f4f6',
      surfaceHover: '#e9ebef',
      surfaceActive: '#dfe2e8',
      border: '#e3e5e9',
      borderStrong: '#cfd3da',
      text: '#1f2328',
      textMuted: '#59606b',
      textFaint: '#8b929d',
      accent: '#2f6fed',
      accentHover: '#1f5fdd',
      accentFg: '#ffffff',
      danger: '#d43b40',
      warning: '#b7791f',
      success: '#1f8a4c',
      info: '#2f6fed',
      titlebar: '#eef0f3',
      titlebarFg: '#3b4048',
      sidebar: '#f7f8fa',
      tabActive: '#ffffff',
      tabHover: '#e6e8ec',
      overlay: 'rgba(15,20,30,0.35)',
      scrollbar: 'rgba(0,0,0,0.18)',
      scrollbarHover: 'rgba(0,0,0,0.32)'
    }
  }
];
