/**
 * This script detects the platform and sets the appropriate VCPKG_TRIPLET environment variable
 * if it's not already set.
 */

const os = require('os');

if (!process.env.VCPKG_TRIPLET) {
  const platform = os.platform();
  let triplet = 'x64-windows-release';
  
  if (platform === 'linux') {
    triplet = 'x64-linux-release';
  } else if (platform === 'darwin') {
    triplet = 'x64-osx-release';
  }
  
  process.env.VCPKG_TRIPLET = triplet;
  console.log(`Platform detected: ${platform}, setting VCPKG_TRIPLET to ${triplet}`);
}
