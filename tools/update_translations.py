#!/usr/bin/env python3
"""Extract explicit owned UI bindings and audit the Qt TS catalog.

Bindings deliberately mark ownership at construction/setter sites; user names,
notes, paths, enum data and JSON keys are never harvested from live widgets.
Run without --check to add unfinished entries, translate them in Qt Linguist,
then run --check. lrelease in CMake compiles the reviewed TS to QM.
"""
from pathlib import Path
import argparse
import json
import re
import xml.etree.ElementTree as ET
from owned_ui_messages import owned_messages,write_patterns

ROOT=Path(__file__).resolve().parents[1]
L=r'"(?:\\.|[^"\\])*"(?:\s*"(?:\\.|[^"\\])*")*'
def value(source):
    return ''.join(json.loads(m[0]) for m in re.finditer(r'"(?:\\.|[^"\\])*"',source))
def extract():
    found=set()
    found.update(('Jester',message) for message in owned_messages(ROOT))
    patterns=[
        r'\buiText\(\s*('+L+r')',
        r'\btranslated(?:Widget<\w+>|Label|Button|CheckBox|GroupBox)\(\s*('+L+r')',
        r'\btranslatedIconWidget<\w+>\([^,]+,\s*('+L+r')',
        r'\baddTranslated(?:Action|Menu|Row|Item|Tab)\([^,]+,\s*('+L+r')',
        r'\baddTranslatedTab\([^,]+,[^,]+,\s*('+L+r')',
        r'\bbindUiText\([^,]+,\s*'+L+r'\s*,\s*('+L+r')',
        r'\badd\((?:file|edit|view|help|pose|motion|physicsMenu),\s*('+L+r')',
        r'\bcheck\(\s*('+L+r')',
        r'\baddTranslatedIconAction\([^,]+,[^,]+,\s*('+L+r')',
        r'\baddTranslatedButton\([^,]+,\s*('+L+r')',
        r'\brow\(\s*('+L+r')',
    ]
    for path in (ROOT/'Editor/src').rglob('*.cpp'):
        source=path.read_text(encoding='utf-8')
        for pattern in patterns:
            for match in re.finditer(pattern,source):found.add(('Jester',value(match[1])))
        for match in re.finditer(r'\bQT_TRANSLATE_NOOP\("([^"]+)",\s*('+L+r')',source):found.add((match[1],value(match[2])))
        for match in re.finditer(r'\b(?:addTranslatedItems|bindHeaders)\([^,]+,\s*\{(.*?)\}\)',source,re.S):
            for item in re.finditer(L,match[1]):found.add(('Jester',value(item[0])))
    # Explicit source arrays used by owned model headers and enum delegates.
    for relative,start,end in [('ui/motion/analysis_views.cpp','columns =','endResetModel();'),
                               ('ui/curves/graph_editor.cpp','addTranslatedItems(box,index.column()','return box;')]:
        source=(ROOT/'Editor/src'/relative).read_text(encoding='utf-8').split(start,1)[1].split(end,1)[0]
        for item in re.finditer(L,source):found.add(('Jester',value(item[0])))
    # Built-in catalog presentation only; imported/user names remain untouched.
    source=(ROOT/'Editor/src/io/catalog/scene_catalog.cpp').read_text(encoding='utf-8').split('for (QString kind',1)[0]
    for item in re.finditer(L,source):
        text=value(item[0])
        if text and not text.endswith('.h'):found.add(('Jester',text))
    # Field group labels are translated in the SceneSettings context.
    for group in ['Display','Lighting','Environment','Background and floor','Camera','Post-processing']:found.add(('SceneSettings',group))
    return {(ctx,text) for ctx,text in found if re.search('[A-Za-z]',text)}
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--check',action='store_true');args=parser.parse_args()
    path=ROOT/'Editor/translations/jester_ru.ts';tree=ET.parse(path);root=tree.getroot()
    contexts={context.findtext('name'):context for context in root.findall('context')}
    known={(context.findtext('name'),message.findtext('source')):message for context in contexts.values() for message in context.findall('message')}
    missing=sorted(extract()-known.keys());unfinished=[];invalid=[]
    for context,source in missing:
        if context not in contexts:
            contexts[context]=ET.SubElement(root,'context');ET.SubElement(contexts[context],'name').text=context
        message=ET.SubElement(contexts[context],'message');ET.SubElement(message,'source').text=source;ET.SubElement(message,'translation',{'type':'unfinished'})
        known[context,source]=message
    for key,message in known.items():
        translation=message.find('translation')
        if translation is None or translation.get('type')=='unfinished' or not ''.join(translation.itertext()).strip():unfinished.append(key)
        if translation is not None and translation.get('type')!='unfinished':
            expected=set(re.findall(r'%L?[1-9][0-9]?|%n',key[1]))
            for form in translation.findall('numerusform') or [translation]:
                text=''.join(form.itertext())
                if set(re.findall(r'%L?[1-9][0-9]?|%n',text))!=expected or ('??' in text and '??' not in key[1]):invalid.append(key)
    if not args.check:
        write_patterns(ROOT)
        ET.indent(tree,space='  ');tree.write(path,encoding='utf-8',xml_declaration=True)
    print(json.dumps({'ownedSources':len(extract()),'catalogEntries':len(known),'missing':len(missing),'unfinished':len(unfinished),'invalidPlaceholdersOrEncoding':len(invalid)},ensure_ascii=False))
    if args.check and (missing or unfinished or invalid):
        for context,source in missing+unfinished+invalid:print(f'{context}: {source}')
        raise SystemExit(1)
if __name__=='__main__':main()
