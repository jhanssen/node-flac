/*global require,module*/

"use strict";

const bindings = require("bindings")("flac.node");
const { Transform } = require("stream");

const Types = {
    Format: 0,
    Metadata: 1,
    Data: 2,
    Done: 3,
    End: 4
};

// TODO: make the flac decoder handle multiple opened streams
class FlacDecoder extends Transform {
    constructor(options) {
        super(options);

        this._flac = bindings.Open((type, data) => {
            //console.log("flac callback", type, typeof this._done, typeof this._flac);
            switch (type) {
            case Types.Data:
                //console.log("push", data.length);
                this.push(data);
                break;
            case Types.Format:
                this.emit("format", data);
                break;
            case Types.Done:
                let done = this._transformCb;
                this._transformCb = undefined;
                done();
                break;
            case Types.End:
                this._flac = undefined;
                break;
            }
        });
    }

    _transform(chunk, encoding, done) {
        // console.log("want to transform", chunk.length, typeof done);
        this._transformCb = done;
        bindings.Feed(this._flac, chunk);
        //console.log("fed total", this._cnt);
    }
};

module.exports = { FlacDecoder: FlacDecoder };
