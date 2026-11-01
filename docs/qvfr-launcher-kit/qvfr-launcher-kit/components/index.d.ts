import type { ReactNode } from 'react';

export interface Size { width: number; height: number; }
export type RenderingMethod = 'qvfr' | 'tfi';
export type FoveatedMode = 'eye' | 'fixed';
export type OutlineColor = 'teal' | 'magenta' | 'green';

export interface ButtonProps {
  children: ReactNode;
  /** @default 'secondary' */
  variant?: 'secondary' | 'primary' | 'destructive' | 'plain';
  /** 'large' is for the Launch button only. @default 'regular' */
  size?: 'regular' | 'large';
  disabled?: boolean;
  onClick?: () => void;
}

export interface CheckboxProps {
  label: ReactNode;
  description?: ReactNode;
  checked?: boolean;
  defaultChecked?: boolean;
  disabled?: boolean;
  onChange?: (checked: boolean) => void;
}

export interface PopUpOption { value: string; label: string; disabled?: boolean; }
export interface PopUpButtonProps {
  options: PopUpOption[];
  value?: string;
  defaultValue?: string;
  onChange?: (value: string) => void;
  'aria-label'?: string;
  'aria-labelledby'?: string;
  /** Minimum width in px. @default 180 */
  width?: number;
  disabled?: boolean;
}

export interface SegmentOption { value: string; label: string; /** CSS color, e.g. 'var(--overlay-teal)' */ swatch?: string; }
export interface SegmentedControlProps {
  options: SegmentOption[];
  value?: string;
  defaultValue?: string;
  onChange?: (value: string) => void;
  'aria-label': string;
  disabled?: boolean;
}

export interface PercentSliderProps {
  /** Percent. */
  value?: number;
  defaultValue?: number;
  onChange?: (percent: number) => void;
  'aria-label'?: string;
  'aria-labelledby'?: string;
  /** @default 0 */
  min?: number;
  /** @default 100 */
  max?: number;
  /** @default 1 */
  step?: number;
  /** Line under the slider, e.g. the resulting pixel size. */
  detail?: ReactNode | ((percent: number) => ReactNode);
  /** @default 240 */
  width?: number;
  disabled?: boolean;
}

export interface FormSectionProps { title?: ReactNode; footer?: ReactNode; children: ReactNode; }
export interface FormRowProps { label?: ReactNode; description?: ReactNode; labelId?: string; disabled?: boolean; children: ReactNode; }

export interface SidebarItem { id: string; label: string; icon: 'layers' | 'display' | 'eye' | 'target' | 'info'; /** Accessible text for a warning glyph. */ attention?: string; }
export interface SidebarProps { items: SidebarItem[]; selected: string; onSelect?: (id: string) => void; heading?: ReactNode; 'aria-label'?: string; }

export interface WindowChromeProps {
  title: ReactNode;
  children: ReactNode;
  width?: number | string;
  height?: number | string;
  toolbar?: ReactNode;
  /** @default 'mac' */
  controls?: 'mac' | 'windows';
}

export interface StatusBadgeProps { children: ReactNode; /** @default 'neutral' */ tone?: 'success' | 'warning' | 'danger' | 'neutral'; live?: boolean; }

export interface FoveationPreviewProps {
  /** @default {width:1292,height:1080} */
  outer?: Size;
  /** @default {width:1008,height:843} */
  inner?: Size;
  /** Percent of the outer view. @default {width:45,height:40} */
  focusSize?: Size;
  method?: RenderingMethod;
  mode?: FoveatedMode;
  /** 0–1 within the view; eye mode only. */
  gaze?: { x: number; y: number };
  showOutline?: boolean;
  outlineColor?: OutlineColor;
  edgeBlend?: boolean;
  feather?: boolean;
  caption?: boolean;
}

export interface LauncherSettings {
  method: RenderingMethod;
  mode: FoveatedMode;
  /** Outer resolution, percent of recommended (20–100). */
  outer: number;
  /** Inner resolution: focus pixel density, percent of recommended (50–200). */
  inner: number;
  /** Foveated size, percent of the outer view (10–100 each). */
  size: Size;
  outline: boolean;
  outlineColor: OutlineColor;
  blend: boolean;
  feather: boolean;
}
export interface LauncherWindowProps {
  /** Headset's recommended per-eye resolution from the OpenXR runtime. @default {width:2584,height:2160} (example) */
  recommendedResolution?: Size;
  defaultSettings?: Partial<LauncherSettings>;
  defaultPane?: 'rendering' | 'resolution' | 'foveation' | 'overlay' | 'about';
  /** @default true */
  eyeTrackingAvailable?: boolean;
  onChange?: (s: LauncherSettings) => void;
  onLaunch?: (s: LauncherSettings) => void;
  width?: number | string;
  height?: number | string;
  controls?: 'mac' | 'windows';
  version?: string;
}

export declare function Button(p: ButtonProps): JSX.Element;
export declare function Checkbox(p: CheckboxProps): JSX.Element;
export declare function PopUpButton(p: PopUpButtonProps): JSX.Element;
export declare function SegmentedControl(p: SegmentedControlProps): JSX.Element;
export declare function PercentSlider(p: PercentSliderProps): JSX.Element;
export declare function FormSection(p: FormSectionProps): JSX.Element;
export declare function FormRow(p: FormRowProps): JSX.Element;
export declare function Sidebar(p: SidebarProps): JSX.Element;
export declare function WindowChrome(p: WindowChromeProps): JSX.Element;
export declare function StatusBadge(p: StatusBadgeProps): JSX.Element;
export declare function FoveationPreview(p: FoveationPreviewProps): JSX.Element;
export declare function LauncherWindow(p: LauncherWindowProps): JSX.Element;
export declare const defaults: LauncherSettings;
export declare const ranges: { outer: { min: number; max: number }; inner: { min: number; max: number }; size: { min: number; max: number } };
/** Outer px = recommended × outer%. Focus px = recommended × inner% × size%. */
export declare function viewPixels(s: LauncherSettings, recommended: Size): { outer: Size; inner: Size };
