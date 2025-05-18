/**
 * This script detects the platform and sets the appropriate VCPKG_TRIPLET environment variable
 * if it's not already set.
 */

const os = require('os');
const { spawn } = require('child_process');
const path = require('path');

if (!process.env.VCPKG_DEFAULT_TRIPLET) {
  const platform = os.platform();
  let triplet = 'x64-windows-release';
 
  if (platform === 'linux') {
    triplet = 'x64-linux-release';
  } else if (platform === 'darwin') {
    triplet = 'x64-osx-release';
  }
 
  process.env.VCPKG_DEFAULT_TRIPLET = triplet;
  console.log(`Platform detected: ${platform}, setting VCPKG_DEFAULT_TRIPLET to ${triplet}`);
}

// Define build directory
const buildDir = path.resolve('/workspace/build');
console.log(`Setting library output directory to: ${buildDir}`);

// Set CMAKE_ARGS environment variable which cmake-js will pass directly to CMake
process.env.CMAKE_ARGS = `-DCMAKE_LIBRARY_OUTPUT_DIRECTORY="${buildDir}" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON`;

// Run cmake-js compile with the detected triplet
console.log(`Running cmake-js compile with VCPKG_DEFAULT_TRIPLET=${process.env.VCPKG_DEFAULT_TRIPLET}`);
console.log(`CMAKE_ARGS=${process.env.CMAKE_ARGS}`);

const cmakeArgs = [
  'compile',
  '--prefer-make',
  `--CDVCPKG_HOST_TRIPLET=${process.env.VCPKG_DEFAULT_TRIPLET}`,
  `--CDVCPKG_TARGET_TRIPLET=${process.env.VCPKG_DEFAULT_TRIPLET}`,
  '--CDCMAKE_TOOLCHAIN_FILE=./vcpkg/scripts/buildsystems/vcpkg.cmake'
];

const cmakeProcess = spawn('cmake-js', cmakeArgs, { stdio: 'inherit', shell: true });

cmakeProcess.on('exit', (code) => {
  process.exit(code);
});
