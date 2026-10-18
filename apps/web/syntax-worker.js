/* Prism is presentation only. Never return HTML to the UI. */
globalThis.Prism = {manual:true, disableWorkerMessageHandler:true};
importScripts('prism-vendor.js');
onmessage = ({data:{id, text, language}}) => {
  try {
    const grammar = Prism.languages[language];
    if (!grammar || text.length > 524288) throw new Error('Plain text fallback');
    const env = {code:text, grammar, language};
    Prism.hooks.run('before-tokenize', env);
    env.tokens = Prism.tokenize(env.code, env.grammar);
    Prism.hooks.run('after-tokenize', env);
    const lines = [[]], endings = [];
    let count = 0;
    const append = (text, type) => {
      const parts = text.split(/(\r\n|\n|\r)/);
      for (let i = 0; i < parts.length; i++) {
        if (i % 2) { endings.push(parts[i]); lines.push([]); continue; }
        if (!parts[i]) continue;
        const line = lines.at(-1), last = line.at(-1);
        if (last && last[1] === type) last[0] += parts[i];
        else { if (++count > 100000) throw new Error('Token budget'); line.push([parts[i], type]); }
      }
    };
    const walk = (value, inherited = '') => {
      if (typeof value === 'string') append(value, inherited);
      else if (Array.isArray(value)) for (const child of value) walk(child, inherited);
      else walk(value.content, /^[a-z][a-z0-9-]*$/.test(value.type) ? value.type : inherited);
    };
    walk(env.tokens);
    if (lines.map((line,i)=>line.map(part=>part[0]).join('') + (endings[i] || '')).join('') !== text) throw new Error('Text changed');
    postMessage({id, result:{lines, endings}});
  } catch (_) { postMessage({id, result:null}); }
};
