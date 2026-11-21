import { createRouter, createWebHistory } from 'vue-router'
import { t } from './i18n'

const routes = [
  { path: '/', name: 'dashboard', component: () => import('./views/DashboardView.vue'), meta: { title: '总览', subtitle: '本地模型工作台' } },
  { path: '/models', name: 'models', component: () => import('./views/ModelsView.vue'), meta: { title: '模型管理', subtitle: '导入、检查与组织本地模型' } },
  { path: '/convert', name: 'convert', component: () => import('./views/ConvertView.vue'), meta: { title: '模型转换', subtitle: 'Safetensors → GGUF 与量化' } },
  { path: '/tasks', name: 'tasks', component: () => import('./views/TasksView.vue'), meta: { title: '批量任务', subtitle: '队列、调度与日志' } },
  { path: '/lab', name: 'lab', component: () => import('./views/LabView.vue'), meta: { title: '量化实验室', subtitle: '多方案体积、性能与结果对比' } },
  { path: '/analyzer', name: 'analyzer', component: () => import('./views/AnalyzerView.vue'), meta: { title: '模型分析', subtitle: '结构、兼容性与完整性' } },
  { path: '/presets', name: 'presets', component: () => import('./views/PresetsView.vue'), meta: { title: '配置预设', subtitle: '精度、量化与验证的可复用组合' } },
  { path: '/tester', name: 'tester', component: () => import('./views/TesterView.vue'), meta: { title: '模型测试', subtitle: '真实 llama-cli 推理' } },
  { path: '/gguf', name: 'gguf', component: () => import('./views/GgufEditorView.vue'), meta: { title: 'GGUF 编辑器', subtitle: '元数据与张量检查' } },
  { path: '/workflows', name: 'workflows', component: () => import('./views/WorkflowsView.vue'), meta: { title: '工作流', subtitle: '可复用的转换流水线' } },
  { path: '/history', name: 'history', component: () => import('./views/HistoryView.vue'), meta: { title: '转换历史', subtitle: '操作时间线与任务轨迹' } },
  { path: '/downloads', name: 'downloads', component: () => import('./views/DownloadsView.vue'), meta: { title: '下载管理', subtitle: 'Hugging Face 模型获取' } },
  { path: '/watch', name: 'watch', component: () => import('./views/WatchView.vue'), meta: { title: '文件夹监控', subtitle: '新模型自动导入与转换' } },
  { path: '/runtime', name: 'runtime', component: () => import('./views/LlamaRuntimeView.vue'), meta: { title: '运行引擎', subtitle: 'llama.cpp 官方发行版与更新' } },
  { path: '/plugins', name: 'plugins', component: () => import('./views/PluginsView.vue'), meta: { title: '插件管理', subtitle: '转换引擎与扩展能力' } },
  { path: '/settings', name: 'settings', component: () => import('./views/SettingsView.vue'), meta: { title: '系统设置', subtitle: '工具链、目录与服务配置' } },
  { path: '/:pathMatch(.*)*', redirect: '/' }
]

export const router = createRouter({
  history: createWebHistory(),
  routes,
  scrollBehavior: () => ({ top: 0 })
})

router.afterEach((to) => {
  const title = (to.meta.title as string) || 'ModelForge'
  document.title = `${t(title)} · ModelForge`
})
