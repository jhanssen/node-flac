/*global require,process,setTimeout*/

const { FlacDecoder } = require("..");
const { createReadStream } = require("fs");
const { Writable } = require("stream");
const Speaker = require("speaker");

//console.log(FlacDecoder);

function play() {
    function wrapSpeaker(speaker) {
        speaker.removeListener('finish', speaker._flush);
        speaker._flush = () => {
            speaker.emit('flush');
            setTimeout(() => {
                speaker.close(true);

                process.nextTick(play);
            }, 500);
        };
        speaker.on('finish', speaker._flush);
        return speaker;
    }

    createReadStream("/tmp/test.flac")
        .pipe(new FlacDecoder)
        .on("format", console.log)
        .pipe(wrapSpeaker(new Speaker));
}

play();
