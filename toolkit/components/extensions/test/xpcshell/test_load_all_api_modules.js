"use strict";

const { Schemas } = ChromeUtils.importESModule(
  "resource://gre/modules/Schemas.sys.mjs"
);

const BASE_SCHEMA = "chrome://extensions/content/schemas/manifest.json";

const CATEGORY_EXTENSION_MODULES = "webextension-modules";
const CATEGORY_EXTENSION_SCHEMAS = "webextension-schemas";
const CATEGORY_EXTENSION_SCRIPTS = "webextension-scripts";

const CATEGORY_EXTENSION_SCRIPTS_ADDON = "webextension-scripts-addon";
const CATEGORY_EXTENSION_SCRIPTS_CONTENT = "webextension-scripts-content";
const CATEGORY_EXTENSION_SCRIPTS_DEVTOOLS = "webextension-scripts-devtools";

let schemaURLs = new Set();
schemaURLs.add("chrome://extensions/content/schemas/experiments.json");

// Helper class used to load the API modules similarly to the apiManager
// defined in ExtensionParent.sys.mjs.
class FakeAPIManager extends ExtensionCommon.SchemaAPIManager {
  constructor(processType = "main") {
    super(processType, Schemas);
    this.initialized = false;
  }

  getModuleJSONURLs() {
    return Array.from(
      Services.catMan.enumerateCategory(CATEGORY_EXTENSION_MODULES),
      ({ value }) => value
    );
  }

  async lazyInit() {
    if (this.initialized) {
      return;
    }

    this.initialized = true;

    let modulesPromise = this.loadModuleJSON(this.getModuleJSONURLs());

    let scriptURLs = [];
    for (let { value } of Services.catMan.enumerateCategory(
      CATEGORY_EXTENSION_SCRIPTS
    )) {
      scriptURLs.push(value);
    }

    let scripts = await Promise.all(
      scriptURLs.map(url => ChromeUtils.compileScript(url))
    );

    this.initModuleData(await modulesPromise);

    this.initGlobal();
    for (let script of scripts) {
      script.executeInGlobal(this.global);
    }

    // Load order matters here. The base manifest defines types which are
    // extended by other schemas, so needs to be loaded first.
    await Schemas.load(BASE_SCHEMA).then(() => {
      let promises = [];
      for (let { value } of Services.catMan.enumerateCategory(
        CATEGORY_EXTENSION_SCHEMAS
      )) {
        promises.push(Schemas.load(value));
      }
      for (let [url, { content }] of this.schemaURLs) {
        promises.push(Schemas.load(url, content));
      }
      for (let url of schemaURLs) {
        promises.push(Schemas.load(url));
      }
      return Promise.all(promises).then(() => {
        Schemas.updateSharedSchemas();
      });
    });
  }

  getAllApiModuleNames(reverseOrder = false) {
    let apiModuleNames = Array.from(this.modules.keys())
      .filter(moduleName => {
        let moduleDesc = this.modules.get(moduleName);
        return moduleDesc && !!moduleDesc.url;
      })
      .sort();

    apiModuleNames = reverseOrder ? apiModuleNames.reverse() : apiModuleNames;
    return apiModuleNames;
  }

  async loadAllModules(reverseOrder = false) {
    await this.lazyInit();

    let apiModuleNames = this.getAllApiModuleNames(reverseOrder);

    for (let apiModule of apiModuleNames) {
      info(
        `Loading apiModule ${apiModule}: ${this.modules.get(apiModule).url}`
      );
      await this.asyncLoadModule(apiModule);
    }
  }
}

// Specialized helper class used to test loading "child process" modules (similarly to the
// SchemaAPIManagers sub-classes defined in ExtensionPageChild.sys.mjs and
// ExtensionContent.sys.mjs).
class FakeChildProcessAPIManager extends FakeAPIManager {
  constructor({ processType, categoryScripts }) {
    super(processType, Schemas);

    this.categoryScripts = categoryScripts;
  }

  async lazyInit() {
    if (!this.initialized) {
      this.initialized = true;
      this.initGlobal();
      for (let { value } of Services.catMan.enumerateCategory(
        this.categoryScripts
      )) {
        await this.loadScript(value);
      }
    }
  }
}

async function test_loading_api_modules(createAPIManager) {
  let fakeAPIManager;

  info("Load API modules in alphabetic order");

  fakeAPIManager = createAPIManager();
  await fakeAPIManager.loadAllModules();

  info("Load API modules in reverse order");

  fakeAPIManager = createAPIManager();
  await fakeAPIManager.loadAllModules(true);
}

add_task(async function test_loading_main_process_sync_load_module() {
  let fakeAPIManager = new FakeAPIManager();
  await fakeAPIManager.lazyInit();

  let names = fakeAPIManager.getAllApiModuleNames();

  // Regression test for bug 1905153 - calling the synchronous loadModule method
  // after asyncLoadModule before the completion of asyncLoadModule should work
  // without errors.
  let promisesBefore = names.map(name => fakeAPIManager.asyncLoadModule(name));
  let syncResults = names.map(name => fakeAPIManager.loadModule(name));
  let promisesAfter = names.map(name => fakeAPIManager.asyncLoadModule(name));

  // Sanity check: loadModule and asyncLoadModule should return the same module,
  // since calling them consecutively.
  for (let [i, name] of names.entries()) {
    let asyncResultBefore = await promisesBefore[i];
    let syncResult = syncResults[i];
    let asyncResultAfter = await promisesAfter[i];
    // The assertion messages could become rather long if we use equal(), so we
    // use ok(===) instead, and for debugging print the values if they are
    // unexpectedly different.
    if (asyncResultBefore !== syncResult || syncResult !== asyncResultAfter) {
      info(`asyncResultBefore: ${uneval(asyncResultBefore)}`);
      info(`syncResult: ${uneval(syncResult)}`);
      info(`asyncResultAfter: ${uneval(asyncResultAfter)}`);
    }
    // eslint-disable-next-line mozilla/no-comparison-or-assignment-inside-ok
    ok(
      asyncResultBefore === syncResult,
      `loadModule() after asyncLoadModule() should return the same for: ${name}`
    );
    // eslint-disable-next-line mozilla/no-comparison-or-assignment-inside-ok
    ok(
      syncResult === asyncResultAfter,
      `asyncLoadModule() after loadModule() should return the same for: ${name}`
    );
  }
});

add_task(function test_loading_main_process_api_modules() {
  return test_loading_api_modules(() => {
    return new FakeAPIManager();
  });
});

add_task(function test_loading_extension_process_modules() {
  return test_loading_api_modules(() => {
    return new FakeChildProcessAPIManager({
      processType: "addon",
      categoryScripts: CATEGORY_EXTENSION_SCRIPTS_ADDON,
    });
  });
});

add_task(function test_loading_devtools_modules() {
  return test_loading_api_modules(() => {
    return new FakeChildProcessAPIManager({
      processType: "devtools",
      categoryScripts: CATEGORY_EXTENSION_SCRIPTS_DEVTOOLS,
    });
  });
});

add_task(async function test_loading_content_process_modules() {
  return test_loading_api_modules(() => {
    return new FakeChildProcessAPIManager({
      processType: "content",
      categoryScripts: CATEGORY_EXTENSION_SCRIPTS_CONTENT,
    });
  });
});
