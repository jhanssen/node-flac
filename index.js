/*global require*/

"use strict";

const bindings = require("bindings")("flac.node");
const { Transform } = require("stream");

// TODO: make the flac decoder handle multiple opened streams
class FlacDecoder extends Transform {
    constructor(options) {
        super(options);

        this._flac = bindings.Open((type, data) => {
        });
    }

    _transform(chunk, encoding, done) {

    }
};

export FlacDecoder;
