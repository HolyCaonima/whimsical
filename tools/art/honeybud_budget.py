"""Triangle budgets apply to a complete kit, summing every material primitive."""
from collections import Counter


def audit(manifest):
    counts = Counter(item['kit'] for item in manifest['placements'])
    models = []
    for name, parts in manifest['kits'].items():
        triangles = sum(part['triangles'] for part in parts)
        budget = 60000 if name == 'cottage' else 20000
        models.append(dict(model=name, triangles=triangles, budget=budget,
                           instances=counts[name], sceneTriangles=triangles * counts[name]))
    violations = [m for m in models if m['triangles'] > m['budget']]
    if violations:
        raise ValueError('Complete model triangle budget exceeded: ' + str(violations))
    return dict(unit='triangles', hardModelLimit=100000, typicalModelLimit=20000,
                uniqueTriangles=sum(m['triangles'] for m in models),
                sceneTriangles=sum(m['sceneTriangles'] for m in models), models=models)
