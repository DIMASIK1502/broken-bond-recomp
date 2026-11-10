// Local first-run import. Files are copied from the user's disk into OPFS.
// Nothing is uploaded and nothing is downloaded.

const REQUIRED = ['default.xex'];

export function safeRelative(path) {
  if (typeof path !== 'string' || path.length === 0 || path.length > 4096 ||
      path.includes('\\') || path.includes(':') || path.includes('\0') ||
      path.split('/').some(part => !part || part === '.' || part === '..')) {
    throw new Error(`Unsafe import path: ${path}`);
  }
  return path;
}

export function folderFiles(files) {
  const entries = files.map(file => {
    const path = file.importPath || file.webkitRelativePath || file.name;
    return {file, path: safeRelative(path)};
  });
  const prefix = entries[0]?.path.split('/')[0] + '/';
  if (!entries.some(entry => entry.path === 'default.xex') &&
      entries.every(entry => entry.path.startsWith(prefix))) {
    for (const entry of entries) entry.path = safeRelative(entry.path.slice(prefix.length));
  }
  const paths = new Set();
  for (const {path} of entries) {
    if (paths.has(path)) throw new Error(`Duplicate import path: ${path}`);
    paths.add(path);
  }
  for (const name of REQUIRED) {
    if (!entries.some(entry => entry.path === name && entry.file.size > 0)) {
      throw new Error(`Select the extracted game folder (missing ${name})`);
    }
  }
  return entries;
}

async function writeFile(directory, path, file) {
  const parts = safeRelative(path).split('/');
  for (const part of parts.slice(0, -1)) {
    directory = await directory.getDirectoryHandle(part, {create: true});
  }
  const handle = await directory.getFileHandle(parts.at(-1), {create: true});
  await file.stream().pipeTo(await handle.createWritable());
}

async function removeIfPresent(root, name) {
  try { await root.removeEntry(name, {recursive: true}); }
  catch (error) { if (error.name !== 'NotFoundError') throw error; }
}

async function hasImport(root) {
  try {
    const marker = await (await root.getFileHandle('import-ready')).getFile();
    if ((await marker.text()).trim() !== '1') return false;
    const game = await root.getDirectoryHandle('game');
    const xex = await (await game.getFileHandle('default.xex')).getFile();
    return xex.size > 0;
  } catch (error) {
    if (error.name === 'NotFoundError' || error.name === 'TypeMismatchError') return false;
    throw error;
  }
}

async function droppedFiles(items) {
  const files = [];
  async function visit(handle, prefix = '') {
    if (handle.kind === 'file') {
      const file = await handle.getFile();
      Object.defineProperty(file, 'importPath', {value: prefix + file.name});
      files.push(file);
    } else {
      for await (const child of handle.values()) await visit(child, prefix + handle.name + '/');
    }
  }
  const handles = items.map(item => item.getAsFileSystemHandle?.());
  for (const promise of handles) {
    const handle = await promise;
    if (handle) await visit(handle);
  }
  return files;
}

async function prepareFiles() {
  const panel = document.getElementById('import-panel');
  const status = window.brokenBond.filesStatus;
  const root = await (await navigator.storage.getDirectory()).getDirectoryHandle('broken_bond', {create: true});
  if (await hasImport(root)) return;
  panel.hidden = false;
  status('Choose your extracted game folder. Files stay in this browser.');
  await new Promise(resolve => {
    let busy = false;
    async function importFiles(files) {
      if (busy || !files.length) return;
      busy = true;
      try {
        const entries = folderFiles(files);
        status('Copying game files to browser storage...');
        await removeIfPresent(root, 'import-ready');
        await removeIfPresent(root, 'game');
        const game = await root.getDirectoryHandle('game', {create: true});
        for (let i = 0; i < entries.length; ++i) {
          status(`Copying ${i + 1}/${entries.length}: ${entries[i].path}`);
          await writeFile(game, entries[i].path, entries[i].file);
        }
        const marker = await root.getFileHandle('import-ready', {create: true});
        const writer = await marker.createWritable();
        await writer.write('1\n');
        await writer.close();
        const persistent = await navigator.storage.persist();
        window.brokenBond.storagePersistent = persistent;
        if (!persistent) console.warn('The browser may evict game data when storage is low.');
        status('Checking imported game...');
        resolve();
      } catch (error) {
        status(`Import failed: ${error.message}. Choose the folder again.`, true);
        busy = false;
      }
    }
    document.getElementById('folder-files').onchange = event => importFiles([...event.target.files]);
    panel.ondragover = event => { event.preventDefault(); };
    panel.ondrop = async event => {
      event.preventDefault();
      try {
        const items = [...event.dataTransfer.items].filter(item => item.kind === 'file');
        const files = items.some(item => item.getAsFileSystemHandle) ?
          await droppedFiles(items) : [...event.dataTransfer.files];
        await importFiles(files);
      } catch (error) { status(`Import failed: ${error.message}`, true); }
    };
  });
}

export async function startWithFiles(start) {
  if (!navigator.storage?.getDirectory || !navigator.locks) {
    throw new Error('This browser needs OPFS and Web Locks. Use current desktop Chrome or Edge on localhost.');
  }
  await navigator.locks.request('broken-bond-storage', {ifAvailable: true}, async lock => {
    if (!lock) throw new Error('Broken Bond is already open in another tab. Close it first.');
    await prepareFiles();
    start();
    await new Promise(() => {});
  });
}
