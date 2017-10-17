/*global require,process*/

const { FlacDecoder } = require("..");
const { createReadStream } = require("fs");
const { Writable } = require("stream");
const Speaker = require("speaker");

//console.log(FlacDecoder);

createReadStream("/tmp/test.flac")
    .on("end", () => { console.log("file end."); })
    .pipe(new FlacDecoder)
    .on("format", console.log)
    .pipe(new Speaker);
