// Shared modal layering state.
//
// Every AppModal is teleported to <body> and the stylesheet gives all backdrops
// the same z-index. A page can mount several dialogs (an import dialog plus the
// directory picker it opens), and Teleport keeps each anchor where the component
// was first mounted, so DOM order does not guarantee that the newest dialog is on
// top. This module is imported once per bundle and tracks a single stack.
const stack: number[] = []
let nextId = 1
let previousOverflow = ''
let scrollLocked = false

function lockScroll() {
  if (typeof document === 'undefined' || scrollLocked) return
  previousOverflow = document.body.style.overflow
  document.body.style.overflow = 'hidden'
  scrollLocked = true
}

function unlockScrollIfIdle() {
  if (typeof document === 'undefined' || !scrollLocked || stack.length > 0) return
  document.body.style.overflow = previousOverflow
  scrollLocked = false
}

/** Allocates a process-wide unique dialog id. */
export function createModalId(): number {
  return nextId++
}

/** Pushes a dialog and returns the z-index it must use. */
export function pushModal(id: number): number {
  const existing = stack.indexOf(id)
  if (existing >= 0) stack.splice(existing, 1)
  stack.push(id)
  lockScroll()
  return 100 + stack.length * 10
}

/** Removes a dialog; scroll is unlocked only when no dialog remains. */
export function popModal(id: number) {
  const existing = stack.indexOf(id)
  if (existing >= 0) stack.splice(existing, 1)
  unlockScrollIfIdle()
}

/** True only for the dialog that was opened last. */
export function isTopModal(id: number): boolean {
  return stack.length > 0 && stack[stack.length - 1] === id
}
