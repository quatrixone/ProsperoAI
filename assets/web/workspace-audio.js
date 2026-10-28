// SPDX-License-Identifier: GPL-3.0-or-later
(function (root) {
    'use strict';

    const MODE_PURPOSE = {
        text: 'text-to-text', audio: 'text-to-audio',
        sd: 'text-to-image', tts: 'text-to-speech',
    };
    const AUDIO_DIR = '/download0/ProsperoAI/work/audio/';

    function filterModels(models, mode) {
        const purpose = MODE_PURPOSE[mode];
        return purpose ? models.filter(model => model.purpose === purpose) : [];
    }

    function createWorkspaceMode({models = [], selectModel, prepareModel, loadedModel = ''} = {}) {
        let mode = 'text';
        let stagedModel = '';
        let options = {};
        return {
            get mode() { return mode; },
            get model() { return stagedModel; },
            loadedModel,
            setModels(value) { models = value; },
            setMode(value) {
                if (!MODE_PURPOSE[value]) throw new Error('Unknown workspace mode.');
                mode = value;
            },
            stageModel(id) { stagedModel = id || ''; },
            setOptions(value) { options = {...value}; },
            async submit(payload, send) {
                const selected = models.find(item => item.id === stagedModel);
                if (!selected || selected.purpose !== MODE_PURPOSE[mode])
                    throw new Error(`Choose an installed ${mode} model before submitting.`);
                await selectModel(stagedModel);
                await prepareModel(stagedModel);
                return send({...options, ...(payload || {}), model: stagedModel});
            },
        };
    }

    function createAudioQueue({run, onChange = () => {}, initialItems = []} = {}) {
        const items = Array.isArray(initialItems) ? initialItems.map(item => ({...item,
            status: item.status === 'running' ? 'failed' : item.status || 'pending',
            note: item.status === 'running' ? 'Check running jobs and the library before retrying' : item.note || '',
        })) : [];
        let running = false;
        let stopRequested = false;
        const changed = () => onChange(items, running);
        return {
            items,
            get running() { return running; },
            add(params, count, seed) {
                const firstSeed = Math.max(1, Math.min(4294967295, Number(seed) || 42));
                const total = Math.min(Math.max(1, Math.min(20, Number(count) || 1)), 4294967295 - firstSeed + 1);
                for (let index = 0; index < total; index++)
                    items.push({id: `${Date.now()}:${index}`, seed: Math.min(4294967295, firstSeed + index),
                        params: {...params}, status: 'pending', note: ''});
                changed();
            },
            async run() {
                if (running) return;
                const pending = items.filter(item => item.status === 'pending');
                if (!pending.length) return;
                running = true;
                stopRequested = false;
                changed();
                for (const item of pending) {
                    if (stopRequested) break;
                    item.status = 'running';
                    item.note = '';
                    changed();
                    try {
                        await run(item);
                        item.status = 'done';
                        item.note = 'Complete';
                    } catch (error) {
                        item.status = 'failed';
                        item.note = String(error?.message || error).slice(0, 140);
                        changed();
                        break;
                    }
                    changed();
                }
                running = false;
                changed();
            },
            stop() { if (running) stopRequested = true; },
            clear() { if (running) return; items.splice(0); changed(); },
        };
    }

    function mount({api, onSubmit, onComplete, getModel, getModels}) {
        const $ = id => document.getElementById(id);
        const local = {
            get(key, fallback) {
                try { return JSON.parse(localStorage.getItem(key)) ?? fallback; } catch { return fallback; }
            },
            set(key, value) { try { localStorage.setItem(key, JSON.stringify(value)); } catch {} },
        };
        const request = async (path, body, method = 'POST', headers = {}) => {
            const response = await api(path, body === undefined ? {method} : {
                method, headers, body: headers['Content-Type'] === 'application/octet-stream'
                    ? body : JSON.stringify(body),
            });
            return response.json();
        };
        const formKey = 'prospero-music-form-v1';
        const formIds = ['audio-lyrics', 'audio-style', 'audio-abc', 'audio-abc-name',
            'audio-duration', 'audio-seed', 'audio-track-name'];
        const savedForm = local.get(formKey, {});
        for (const id of formIds) if (savedForm[id] !== undefined) $(id).value = savedForm[id];
        const saveForm = () => local.set(formKey, Object.fromEntries(formIds.map(id => [id, $(id).value])));
        for (const id of formIds) $(id).addEventListener('input', saveForm);

        const status = (message, kind = '') => {
            $('audio-status').textContent = message;
            $('audio-status').className = kind;
        };
        const showProgress = (message, kind = 'busy') => {
            $('audio-status').textContent = message;
            $('audio-status').className = kind;
        };
        const pathFor = name => AUDIO_DIR + encodeURIComponent(name);

        let library = [];
        let queue;
        const queueKey = 'prospero-music-queue-v1';
        const renderQueue = () => {
            const rootElement = $('audio-queue');
            rootElement.replaceChildren();
            for (const item of queue.items) {
                const row = document.createElement('li');
                row.textContent = `Seed ${item.seed}: ${item.status}${item.note ? ` · ${item.note}` : ''}`;
                rootElement.append(row);
            }
            $('audio-queue-run').disabled = queue.running || !queue.items.some(item => item.status === 'pending');
            $('audio-queue-stop').disabled = !queue.running;
            $('audio-queue-clear').disabled = queue.running;
        };
        queue = createAudioQueue({
            initialItems: local.get(queueKey, []),
            onChange(items) { local.set(queueKey, items); renderQueue(); },
            run: item => generate({...item.params, seed: item.seed}, item.seed),
        });

        function renderLibrary() {
            const list = $('audio-library');
            list.replaceChildren();
            const audioFiles = library.filter(file => /\.(wav|mp3)$/i.test(file.name) && !file.meta?.stem_of);
            const sourceFiles = audioFiles.filter(file => /\.wav$/i.test(file.name));
            const abcFiles = library.filter(file => /\.abc$/i.test(file.name));
            const source = $('audio-source');
            source.replaceChildren();
            const placeholder = document.createElement('option');
            placeholder.value = ''; placeholder.textContent = 'Choose an audio file…'; source.append(placeholder);
            for (const file of sourceFiles) {
                const option = document.createElement('option');
                option.value = file.name; option.textContent = file.name; source.append(option);
            }
            const abcSelect = $('audio-abc-library');
            abcSelect.replaceChildren();
            const noAbc = document.createElement('option'); noAbc.value = ''; noAbc.textContent = 'Choose an ABC file…'; abcSelect.append(noAbc);
            for (const file of abcFiles) {
                const option = document.createElement('option'); option.value = file.name;
                option.textContent = file.name; abcSelect.append(option);
            }
            const stemsByParent = new Map();
            for (const file of library) if (file.meta?.stem_of) {
                const stems = stemsByParent.get(file.meta.stem_of) || [];
                stems.push(file); stemsByParent.set(file.meta.stem_of, stems);
            }
            if (!library.length) {
                const empty = document.createElement('p'); empty.className = 'muted';
                empty.textContent = 'Your audio files and ABC scores will appear here.'; list.append(empty);
            }
            for (const file of library.filter(item => !item.meta?.stem_of)) {
                const article = document.createElement('article'); article.className = 'audio-library-item';
                const heading = document.createElement('strong'); heading.textContent = file.name; article.append(heading);
                if (/\.(wav|mp3)$/i.test(file.name)) {
                    const player = document.createElement('audio'); player.controls = true; player.preload = 'none';
                    player.src = pathFor(file.name); article.append(player);
                    const download = document.createElement('a'); download.href = pathFor(file.name);
                    download.download = file.name; download.textContent = 'Download'; article.append(download);
                    if (/\.wav$/i.test(file.name)) {
                        const cover = document.createElement('button'); cover.type = 'button'; cover.className = 'secondary';
                        cover.textContent = 'Use for Cover'; cover.onclick = () => { source.value = file.name; void runSourceWorkflow('cover'); };
                        const stems = document.createElement('button'); stems.type = 'button'; stems.className = 'secondary';
                        stems.textContent = 'Split stems'; stems.onclick = () => { source.value = file.name; void runSourceWorkflow('stems'); };
                        article.append(cover, stems);
                    }
                    for (const stem of stemsByParent.get(file.name) || []) {
                        const stemRow = document.createElement('div'); stemRow.className = 'audio-stem';
                        const stemPlayer = document.createElement('audio'); stemPlayer.controls = true;
                        stemPlayer.preload = 'none'; stemPlayer.src = pathFor(stem.name);
                        const stemLink = document.createElement('a'); stemLink.href = pathFor(stem.name);
                        stemLink.download = stem.name; stemLink.textContent = `Download ${stem.name}`;
                        stemRow.append(stemPlayer, stemLink); article.append(stemRow);
                    }
                } else {
                    const use = document.createElement('button'); use.type = 'button'; use.className = 'secondary';
                    use.textContent = 'Load ABC into editor'; use.onclick = () => void loadAbc(file.name);
                    article.append(use);
                }
                list.append(article);
            }
        }

        async function refreshLibrary() {
            try {
                const result = await request('/api/music/files', undefined, 'GET');
                library = Array.isArray(result.files) ? result.files : [];
                renderLibrary();
            } catch (error) { status(`Could not load audio library: ${error.message}`, 'error'); }
        }

        async function uploadBinary(name, bytes) {
            return request('/api/music/uploadbin', bytes, 'POST', {
                'Content-Type': 'application/octet-stream', 'X-Upload-Name': name,
            });
        }

        async function uploadWav(name, bytes) {
            const chunkSize = 140 * 1024;
            for (let offset = 0, part = 0; offset < bytes.length; offset += chunkSize, part++) {
                const slice = bytes.subarray(offset, Math.min(offset + chunkSize, bytes.length));
                let binary = '';
                for (let i = 0; i < slice.length; i += 0x8000)
                    binary += String.fromCharCode(...slice.subarray(i, Math.min(i + 0x8000, slice.length)));
                await request('/api/music/upload', {name, part, data: btoa(binary)});
            }
        }

        async function localFileToWav(file) {
            const AudioContextType = window.AudioContext || window.webkitAudioContext;
            const OfflineAudioContextType = window.OfflineAudioContext || window.webkitOfflineAudioContext;
            if (!AudioContextType || !OfflineAudioContextType)
                throw new Error('This browser cannot decode audio files. Choose a WAV or MP3 file instead.');
            const context = new AudioContextType();
            let decoded;
            try { decoded = await context.decodeAudioData(await file.arrayBuffer()); }
            finally { void context.close(); }
            const sampleRate = 16000;
            const frames = Math.max(1, Math.ceil(decoded.duration * sampleRate));
            if (frames > 300 * sampleRate) throw new Error('Audio longer than 5 minutes; trim it before Cover.');
            const offline = new OfflineAudioContextType(1, frames, sampleRate);
            const source = offline.createBufferSource(); source.buffer = decoded; source.connect(offline.destination); source.start();
            const rendered = await offline.startRendering(), pcm = rendered.getChannelData(0);
            const view = new DataView(new ArrayBuffer(44 + pcm.length * 2));
            const write = (offset, value) => { for (let i = 0; i < value.length; i++) view.setUint8(offset + i, value.charCodeAt(i)); };
            write(0, 'RIFF'); view.setUint32(4, 36 + pcm.length * 2, true); write(8, 'WAVE'); write(12, 'fmt ');
            view.setUint32(16, 16, true); view.setUint16(20, 1, true); view.setUint16(22, 1, true);
            view.setUint32(24, sampleRate, true); view.setUint32(28, sampleRate * 2, true);
            view.setUint16(32, 2, true); view.setUint16(34, 16, true); write(36, 'data');
            view.setUint32(40, pcm.length * 2, true);
            for (let i = 0; i < pcm.length; i++) view.setInt16(44 + i * 2,
                Math.max(-1, Math.min(1, pcm[i])) * (pcm[i] < 0 ? 32768 : 32767), true);
            return new Uint8Array(view.buffer);
        }

        async function uploadLocalCoverSource() {
            const file = $('audio-upload').files?.[0];
            if (!file) throw new Error('Choose an audio file first.');
            const base = file.name.split(/[\\/]/).pop().replace(/\.[^.]+$/, '')
                .replace(/[^A-Za-z0-9._-]/g, '_').slice(0, 60).toLowerCase() || 'cover';
            if (/\.wav$/i.test(file.name) && file.size <= 64 * 1024 * 1024) {
                await uploadBinary(base + '.wav', await file.arrayBuffer());
                return base + '.wav';
            }
            const wav = await localFileToWav(file), name = base + '-src.wav';
            await uploadWav(name, wav);
            return name;
        }

        async function loadAbc(name) {
            const response = await api(pathFor(name));
            if (!response.ok) throw new Error(`Could not read ${name}.`);
            $('audio-abc').value = await response.text();
            $('audio-abc-name').value = name;
            saveForm();
            status(`Loaded ${name} into the ABC editor.`);
        }

        async function submitAndPoll(model, body, label) {
            const started = Date.now();
            showProgress(`${label}: submitting…`);
            const submitted = await onSubmit(model, body);
            if (!submitted?.job_id) return submitted;
            while (true) {
                await new Promise(resolve => setTimeout(resolve, 2000));
                const state = await request(`/api/jobs?id=${encodeURIComponent(submitted.job_id)}`, undefined, 'GET');
                if (state.state === 'running') {
                    showProgress(`${label}: running · ${state.elapsed_s || Math.floor((Date.now() - started) / 1000)}s`);
                    continue;
                }
                if (!state.result) throw new Error('The job is no longer available. Check the audio library before retrying.');
                if (state.result.error) throw new Error(state.result.error.message || state.result.error);
                return state.result;
            }
        }

        async function runSourceWorkflow(workflow) {
            const name = $('audio-source').value;
            if (!name) { status('Choose an audio file from the library first.', 'error'); return; }
            try {
                const model = workflow === 'cover' ? 'sheetsage2-orig' : 'htdemucs-q8_0';
                const result = await submitAndPoll(model, {
                    audio_workflow: workflow, model, source: name, async: true,
                }, workflow === 'cover' ? 'Cover to ABC' : 'Split into stems');
                if (workflow === 'cover') {
                    const path = String(result.response || '').match(/\/download0\/[^\s,]+\.abc/);
                    const nameFromResult = path?.[0].replace(/["',).]+$/, '').split('/').at(-1);
                    if (!nameFromResult) throw new Error(result.error || 'The transcription did not produce an ABC file.');
                    await loadAbc(nameFromResult);
                    $('audio-abc-name').value = nameFromResult;
                }
                await refreshLibrary();
                status(workflow === 'cover' ? 'ABC is ready to edit.' : 'Stems are ready to play or download.', 'success');
                onComplete?.(result);
            } catch (error) { status(`${workflow === 'cover' ? 'Cover' : 'Stems'} failed: ${error.message}`, 'error'); }
        }

        async function generate(params = {}, seedOverride) {
            const model = getModel();
            if (!model || !filterModels(getModels(), 'audio').some(item => item.id === model))
                throw new Error('Choose an installed audio model before generating.');
            if (!String(params.style || '').trim()) throw new Error('Write a style prompt before generating.');
            if (!String(params.lyrics || '').trim() && !String(params.abc || '').trim())
                throw new Error('Write lyrics or load an ABC score before generating.');
            let abcName = String(params.abc_name || '').trim();
            if (abcName && !/\.abc$/i.test(abcName)) abcName += '.abc';
            const abcContent = String(params.abc || '').trim();
            if (abcContent && abcName) await request('/api/music/abc', {name: abcName, content: params.abc});
            const body = {
                audio_workflow: 'create', model, style: params.style, lyrics: params.lyrics,
                seed: seedOverride ?? (Number(params.seed) || 42),
                duration_seconds: Number(params.duration) || 240,
                track_name: params.track_name || '', abc_name: abcContent || abcName ? abcName : '',
                abc_content: abcContent, async: true,
            };
            const result = await submitAndPoll(model, body, `Music · seed ${body.seed}`);
            if (result.error) throw new Error(result.error.message || result.error);
            await refreshLibrary();
            status(String(result.response || 'Generation complete.'), 'success');
            onComplete?.(result);
            return result;
        }

        function currentParams() {
            return {
                lyrics: $('audio-lyrics').value, style: $('audio-style').value,
                abc: $('audio-abc').value, abc_name: $('audio-abc-name').value,
                duration: $('audio-duration').value, seed: $('audio-seed').value,
                track_name: $('audio-track-name').value,
            };
        }

        $('audio-generate').onclick = async () => {
            const button = $('audio-generate'); button.disabled = true;
            try { await generate(currentParams()); } catch (error) { status(error.message, 'error'); }
            finally { button.disabled = false; }
        };
        $('audio-regen').onclick = () => {
            $('audio-seed').value = String(Math.min(4294967295, (Number($('audio-seed').value) || 42) + 1));
            saveForm(); $('audio-generate').click();
        };
        $('audio-cover').onclick = () => void runSourceWorkflow('cover');
        $('audio-stems').onclick = () => void runSourceWorkflow('stems');
        $('audio-abc-save').onclick = async () => {
            let name = $('audio-abc-name').value.trim() || 'workspace.abc';
            if (!/\.abc$/i.test(name)) name += '.abc';
            try {
                await request('/api/music/abc', {name, content: $('audio-abc').value});
                $('audio-abc-name').value = name; await refreshLibrary();
                status(`Saved ${name}.`, 'success');
            } catch (error) { status(`Could not save ABC: ${error.message}`, 'error'); }
        };
        $('audio-abc-open').onclick = () => $('audio-abc-file').click();
        $('audio-abc-file').onchange = async event => {
            const file = event.target.files?.[0]; event.target.value = '';
            if (!file) return;
            $('audio-abc').value = await file.text();
            $('audio-abc-name').value = file.name.replace(/[^A-Za-z0-9._-]/g, '_').replace(/\.abc$/i, '') + '.abc';
            saveForm();
        };
        $('audio-abc-load').onclick = () => {
            const name = $('audio-abc-library').value || $('audio-abc-name').value;
            if (name) void loadAbc(name).catch(error => status(error.message, 'error'));
        };
        $('audio-abc-clear').onclick = () => { $('audio-abc').value = ''; saveForm(); };
        $('audio-upload-save').onclick = async () => {
            const file = $('audio-upload').files?.[0];
            if (!file) { status('Choose a WAV or MP3 file to upload.', 'error'); return; }
            if (!/\.(wav|mp3)$/i.test(file.name)) { status('Library upload accepts WAV or MP3 files.', 'error'); return; }
            if (file.size > 64 * 1024 * 1024) { status('File is over the 64 MiB library limit.', 'error'); return; }
            const name = file.name.replace(/[^A-Za-z0-9._-]/g, '_');
            try {
                showProgress(`Uploading ${name}…`); await uploadBinary(name, await file.arrayBuffer());
                $('audio-upload').value = ''; await refreshLibrary(); status(`Saved ${name} to the audio library.`, 'success');
            } catch (error) { status(`Upload failed: ${error.message}`, 'error'); }
        };
        $('audio-cover-upload').onclick = async () => {
            const button = $('audio-cover-upload'); button.disabled = true;
            try {
                showProgress('Preparing the uploaded audio for Cover…');
                const source = await uploadLocalCoverSource();
                await refreshLibrary(); $('audio-source').value = source; await runSourceWorkflow('cover');
            } catch (error) { status(`Cover failed: ${error.message}`, 'error'); }
            finally { button.disabled = false; $('audio-upload').value = ''; }
        };
        $('audio-refresh').onclick = () => void refreshLibrary();
        $('audio-duration').oninput = () => { $('audio-duration-value').textContent = $('audio-duration').value; saveForm(); };
        $('audio-seed').onchange = saveForm;
        $('audio-queue-add').onclick = () => {
            queue.add(currentParams(), $('audio-queue-count').value, $('audio-seed').value);
            status('Variants added to the queue.');
        };
        $('audio-queue-run').onclick = () => void queue.run();
        $('audio-queue-stop').onclick = () => { queue.stop(); showProgress('Stopping after the active generation finishes…'); };
        $('audio-queue-clear').onclick = () => queue.clear();
        $('audio-duration-value').textContent = $('audio-duration').value;
        renderQueue();
        void refreshLibrary();
    }

    root.ProsperoWorkspace = {filterModels, createWorkspaceMode, createAudioQueue, mount};
})(globalThis);
