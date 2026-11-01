const fs = require('fs');

const allLines = fs.readFileSync('llhttp.cxx').toString().split('\n')

allLines.forEach((line, idx) => {
  if (line.includes('start = state')) {
    if (allLines[idx - 3].includes('const unsigned char* start')) {
      allLines[idx] = allLines[idx].replace('= state', '= (const unsigned char*)state')
    }
  }
});

fs.writeFileSync('llhttp.cxx', allLines.join('\n'))
