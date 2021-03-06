#include "coreclrhost.h"
#include <dirent.h>
#include <dlfcn.h>
#include <iostream>
#include <limits.h>
#include <set>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <Rcpp.h>

using namespace std;

#ifdef __linux__
const string libcoreclr = "libcoreclr.so";
#elif __APPLE__
const string libcoreclr = "libcoreclr.dylib";
#endif

typedef char *(*bootstrap_ptr)();
typedef double (*plus_ptr)(double x, double y);
typedef double (*sum_ptr)(double *x, int n);

void AddFilesFromDirectoryToTpaList(const char *directory,
  std::string &tpaList) {
  const char *const tpaExtensions[] = {
    ".ni.dll", // Probe for .ni.dll first so that it's preferred if ni and il
    // coexist in the same dir
    ".dll",
    ".ni.exe",
    ".exe",
  };

  DIR *dir = opendir(directory);
  if (dir == nullptr) {
    return;
  }

  std::set<std::string> addedAssemblies;

  // Walk the directory for each extension separately so that we first get files
  // with .ni.dll extension, then files with .dll extension, etc.
  for (int extIndex = 0;
    extIndex < sizeof(tpaExtensions) / sizeof(tpaExtensions[0]);
    extIndex++) {
    const char *ext = tpaExtensions[extIndex];
    int extLength = strlen(ext);

    struct dirent *entry;

    // For all entries in the directory
    while ((entry = readdir(dir)) != nullptr) {
      // We are interested in files only
      switch (entry->d_type) {
      case DT_REG:
        break;

        // Handle symlinks and file systems that do not support d_type
      case DT_LNK:
      case DT_UNKNOWN: {
        std::string fullFilename;

        fullFilename.append(directory);
        fullFilename.append("/");
        fullFilename.append(entry->d_name);

        struct stat sb;
        if (stat(fullFilename.c_str(), &sb) == -1) {
          continue;
        }

        if (!S_ISREG(sb.st_mode)) {
          continue;
        }
      } break;

      default:
        continue;
      }

      std::string filename(entry->d_name);

      // Check if the extension matches the one we are looking for
      int extPos = filename.length() - extLength;
      if ((extPos <= 0) || (filename.compare(extPos, extLength, ext) != 0)) {
        continue;
      }

      std::string filenameWithoutExt(filename.substr(0, extPos));

      // Make sure if we have an assembly with multiple extensions present,
      // we insert only one version of it.
      if (addedAssemblies.find(filenameWithoutExt) == addedAssemblies.end()) {
        addedAssemblies.insert(filenameWithoutExt);

        tpaList.append(directory);
        tpaList.append("/");
        tpaList.append(filename);
        tpaList.append(":");
      }
    }

    // Rewind the directory stream to be able to iterate over it for the next
    // extension
    rewinddir(dir);
  }

  closedir(dir);
}

int run(const char* app_path, const char* pkg_path) {
  cout << "Loading CoreCLR..." << endl;

  //
  // Load CoreCLR
  //
  string coreclr_path(pkg_path);
  coreclr_path.append("/");
  coreclr_path.append(libcoreclr);

  cout << "coreclr_path:" << coreclr_path << endl;

  void *coreclr = dlopen(coreclr_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (coreclr == NULL) {
    cerr << "failed to open " << coreclr_path << endl;
    cerr << "error: " << dlerror() << endl;
    return -1;
  }

  //
  // Initialize CoreCLR
  //
  std::cout << "Initializing CoreCLR..." << endl;

  coreclr_initialize_ptr coreclr_init =
    reinterpret_cast<coreclr_initialize_ptr>(
      dlsym(coreclr, "coreclr_initialize"));
  if (coreclr_init == NULL) {
    cerr << "couldn't find coreclr_initialize in " << coreclr_path << endl;
    return -1;
  }

  cout << "Initialize tpa_list" << endl;
  string tpa_list;
  AddFilesFromDirectoryToTpaList(pkg_path, tpa_list);

  cout << "Initialize properties" << endl;
  const char *property_keys[] = {"APP_PATHS", "TRUSTED_PLATFORM_ASSEMBLIES"};
  const char *property_values[] = {// APP_PATHS
    app_path,
    // TRUSTED_PLATFORM_ASSEMBLIES
    tpa_list.c_str()};

  cout << "coreclr_init" << endl;
  void *coreclr_handle;
  unsigned int domain_id;
  int ret =
    coreclr_init(app_path, // exePath
      "host",   // appDomainFriendlyName
      sizeof(property_values) / sizeof(char *), // propertyCount
      property_keys,                            // propertyKeys
      property_values,                          // propertyValues
      &coreclr_handle,                          // hostHandle
      &domain_id                                // domainId
    );
  if (ret < 0) {
    cerr << "failed to initialize coreclr. cerr = " << ret << endl;
    return -1;
  }

  //
  // Once CoreCLR is initialized, bind to the delegate
  //
  std::cout << "Creating delegate..." << endl;
  coreclr_create_delegate_ptr coreclr_create_dele =
    reinterpret_cast<coreclr_create_delegate_ptr>(
      dlsym(coreclr, "coreclr_create_delegate"));
  if (coreclr_create_dele == NULL) {
    cerr << "couldn't find coreclr_create_delegate in " << coreclr_path << endl;
    return -1;
  }

  {
    // Create the delegate to ManLib::Bootstrap()
    bootstrap_ptr dele_bootstrap;
    ret = coreclr_create_dele(coreclr_handle, domain_id, "manlib", "ManLib",
      "Bootstrap",
      reinterpret_cast<void **>(&dele_bootstrap));
    if (ret < 0) {
      cerr << "couldn't create delegate. err = " << ret << endl;
      return -1;
    }

    // Call the delegate to ManLib::Bootstrap()
    cout << "Calling ManLib::Bootstrap() through delegate..." << endl;

    char *msg = dele_bootstrap();
    cout << "ManLib::Bootstrap() returned " << msg << endl;
    free(msg); // returned string need to be free-ed
  }

  {
    // Create the delegate to ManLib::Plus()
    plus_ptr dele_plus;
    ret = coreclr_create_dele(coreclr_handle, domain_id, "manlib", "ManLib",
      "Plus", reinterpret_cast<void **>(&dele_plus));
    if (ret < 0) {
      cerr << "couldn't create delegate. err = " << ret << endl;
      return -1;
    }

    // Call the delegate to ManLib::Plus()
    cout << "Calling ManLib::Plus() through delegate..." << endl;

    double res = dele_plus(1, 2);
    cout << "ManLib::Plus(1, 2) returned " << res << endl;
  }

  {
    // Create the delegate to ManLib::Sum(double*, int)
    sum_ptr dele_sum;
    ret = coreclr_create_dele(coreclr_handle, domain_id, "manlib", "ManLib",
      "Sum", reinterpret_cast<void **>(&dele_sum));
    if (ret < 0) {
      cerr << "couldn't create delegate. err = " << ret << endl;
      return -1;
    }

    // Call the delegate to ManLib::Sum()
    double nums[10];
    for (int i = 0; i < 10; i++) {
      nums[i] = i;
    }
    cout << "Calling ManLib::Sum() through delegate..." << endl;
    double sum = dele_sum(nums, 10);
    cout << "ManLib::Sum(1:10, 10) returned " << sum << endl;
  }

  // dlclose(coreclr);
}

// [[Rcpp::export]]
void test(const char* app_path, const char* pkg_path) {
  cout << "test starts" << endl;
  run(app_path, pkg_path);
}
