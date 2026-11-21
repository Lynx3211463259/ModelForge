<script setup lang="ts">
const props = withDefaults(defineProps<{ name: string; size?: number; stroke?: number }>(), {
  size: 17,
  stroke: 1.6
})

const paths: Record<string, string[]> = {
  dashboard: ['M3 3h7v7H3z', 'M14 3h7v4h-7z', 'M14 11h7v10h-7z', 'M3 14h7v7H3z'],
  models: ['M12 3l8 4.5v9L12 21l-8-4.5v-9L12 3z', 'M4 7.5l8 4.5 8-4.5', 'M12 12v9'],
  convert: ['M4 7h12', 'M13 4l3 3-3 3', 'M20 17H8', 'M11 14l-3 3 3 3'],
  tasks: ['M8 6h13', 'M8 12h13', 'M8 18h13', 'M3 6h.01', 'M3 12h.01', 'M3 18h.01'],
  lab: ['M9 3h6', 'M10 3v6l-5 8a2 2 0 001.7 3h10.6a2 2 0 001.7-3l-5-8V3', 'M7.5 14h9'],
  analyzer: ['M4 19V9', 'M9 19V5', 'M14 19v-7', 'M19 19V3', 'M2 21h20'],
  tester: ['M21 12a9 9 0 11-3-6.7', 'M21 3v5h-5', 'M8 12l3 3 5-6'],
  gguf: ['M5 3h9l5 5v13H5z', 'M14 3v5h5', 'M8 13h8', 'M8 17h5'],
  workflows: ['M5 4h5v5H5z', 'M14 15h5v5h-5z', 'M10 6.5h4a2 2 0 012 2V15', 'M3 17h4'],
  history: ['M3 12a9 9 0 109-9', 'M3 3v6h6', 'M12 7v5l3 2'],
  downloads: ['M12 3v12', 'M7 10l5 5 5-5', 'M4 19h16'],
  plugins: ['M8 3v4H4v5h4v4h5v4h5v-4h2', 'M18 3h-4v4h-4v5h4v3'],
  settings: ['M12 15.5a3.5 3.5 0 100-7 3.5 3.5 0 000 7z', 'M19.4 15a1.7 1.7 0 00.3 1.9l.1.1a2 2 0 11-2.8 2.8l-.1-.1a1.7 1.7 0 00-1.9-.3 1.7 1.7 0 00-1 1.5V21a2 2 0 11-4 0v-.1a1.7 1.7 0 00-1.1-1.5 1.7 1.7 0 00-1.9.3l-.1.1a2 2 0 11-2.8-2.8l.1-.1a1.7 1.7 0 00.3-1.9 1.7 1.7 0 00-1.5-1H3a2 2 0 110-4h.1A1.7 1.7 0 004.6 8a1.7 1.7 0 00-.3-1.9l-.1-.1a2 2 0 112.8-2.8l.1.1a1.7 1.7 0 001.9.3H9a1.7 1.7 0 001-1.5V2a2 2 0 114 0v.1a1.7 1.7 0 001 1.5 1.7 1.7 0 001.9-.3l.1-.1a2 2 0 112.8 2.8l-.1.1a1.7 1.7 0 00-.3 1.9V8a1.7 1.7 0 001.5 1H22a2 2 0 110 4h-.1a1.7 1.7 0 00-1.5 1z'],
  plus: ['M12 5v14', 'M5 12h14'],
  search: ['M21 21l-4.3-4.3', 'M19 11a8 8 0 11-16 0 8 8 0 0116 0z'],
  refresh: ['M20 11a8 8 0 10-2.3 6.7', 'M20 4v7h-7'],
  play: ['M8 5l11 7-11 7z'],
  stop: ['M6 6h12v12H6z'],
  pause: ['M9 5v14', 'M15 5v14'],
  resume: ['M8 5l11 7-11 7z'],
  trash: ['M4 7h16', 'M9 7V4h6v3', 'M6 7l1 14h10l1-14', 'M10 11v6', 'M14 11v6'],
  retry: ['M3 12a9 9 0 101.7-5.3', 'M3 4v6h6'],
  close: ['M6 6l12 12', 'M18 6L6 18'],
  copy: ['M9 9h11v11H9z', 'M4 15V4h11'],
  download: ['M12 3v12', 'M7 10l5 5 5-5', 'M4 20h16'],
  folder: ['M3 6h6l2 2h10v11H3z'],
  file: ['M6 3h8l4 4v14H6z', 'M14 3v5h5'],
  check: ['M5 12l4 4L19 6'],
  warning: ['M12 3l10 18H2z', 'M12 9v5', 'M12 17.5h.01'],
  info: ['M12 21a9 9 0 100-18 9 9 0 000 18z', 'M12 11v5', 'M12 8h.01'],
  cpu: ['M7 7h10v10H7z', 'M9 2v3', 'M15 2v3', 'M9 19v3', 'M15 19v3', 'M2 9h3', 'M2 15h3', 'M19 9h3', 'M19 15h3'],
  memory: ['M5 6h14v12H5z', 'M8 10h8v4H8z', 'M9 3v3', 'M15 3v3', 'M9 18v3', 'M15 18v3', 'M2 10h3', 'M19 10h3', 'M2 14h3', 'M19 14h3'],
  disk: ['M4 4h16v16H4z', 'M7 8h10', 'M7 12h6', 'M16 16h1'],
  activity: ['M2 12h4l3-8 4 16 3-8h6'],
  terminal: ['M4 5h16v14H4z', 'M7 9l3 3-3 3', 'M13 15h4'],
  arrow: ['M5 12h14', 'M13 6l6 6-6 6'],
  external: ['M14 4h6v6', 'M20 4l-9 9', 'M18 14v6H4V6h6'],
  star: ['M12 3l2.8 5.7 6.2.9-4.5 4.4 1.1 6.2L12 17.3 6.4 20.2l1.1-6.2L3 9.6l6.2-.9z'],
  filter: ['M4 5h16', 'M7 12h10', 'M10 19h4'],
  zap: ['M13 2L4 14h7l-1 8 9-12h-7z'],
  layers: ['M12 3l9 5-9 5-9-5z', 'M3 12l9 5 9-5', 'M3 17l9 5 9-5'],
  clock: ['M12 21a9 9 0 100-18 9 9 0 000 18z', 'M12 7v5l3 2'],
  menu: ['M4 6h16', 'M4 12h16', 'M4 18h16'],
  save: ['M5 3h12l3 3v15H5z', 'M8 3v6h8V3', 'M8 15h8'],
  upload: ['M12 17V5', 'M7 10l5-5 5 5', 'M4 20h16'],
  memory2: ['M12 3a9 9 0 100 18 9 9 0 000-18z', 'M12 8v4l2.5 2.5'],
  dot: ['M12 12h.01'],
  sun: ['M12 17.5a5.5 5.5 0 100-11 5.5 5.5 0 000 11z', 'M12 2v2', 'M12 20v2',
        'M4.2 4.2l1.4 1.4', 'M18.4 18.4l1.4 1.4', 'M2 12h2', 'M20 12h2',
        'M4.2 19.8l1.4-1.4', 'M18.4 5.6l1.4-1.4'],
  moon: ['M21 12.8A9 9 0 1111.2 3a7 7 0 009.8 9.8z']
}
</script>

<template>
  <svg :width="props.size" :height="props.size" viewBox="0 0 24 24" fill="none" aria-hidden="true">
    <path
      v-for="(d, index) in (paths[props.name] || paths.dot)"
      :key="index"
      :d="d"
      stroke="currentColor"
      :stroke-width="props.stroke"
      stroke-linecap="round"
      stroke-linejoin="round"
    />
  </svg>
</template>
